#include "clustering.h"
#include "contiki.h"
#include "fft_processing.h"
#include "lib/list.h"
#include "lib/memb.h"
#include "lib/random.h"
#include "mac_hash.h"
#include "net/ipv6/simple-udp.h"
#include "net/ipv6/uip-ds6.h"
#include "net/ipv6/uip.h"
#include "net/mac/tsch/tsch.h"
#include "net/netstack.h"
#include "net/packetbuf.h"
#include "network_messages.h"
#include "pipeline_phase.h"
#include "pipeline_schedule.h"
#include "sys/log.h"

#include <string.h>

#define LOG_MODULE "SlaveApp"
#define LOG_LEVEL LOG_LEVEL_INFO

#ifndef USE_CSV_TRACES
#    define USE_CSV_TRACES 1
#endif

/* Each connection binds a local/remote port pair. A wildcard remote IP allows
 * messages from more than one neighbor; the application decides which sender to accept.
 */
static struct simple_udp_connection udp_client_conn;
static struct simple_udp_connection udp_server_conn;
static struct simple_udp_connection udp_peer_conn;

typedef struct {
    bool is_active;
    uint16_t node_id;
    unsigned long last_seen;
    bool first_packet_received;
    uint8_t last_seq_no;
} child_state_t;

static child_state_t connected_children[MAX_CHILDREN];
static uint16_t my_slave_id;

typedef enum { STATE_DISCOVERING, STATE_WAITING_OFFER, STATE_BOUND } slave_state_t;
static slave_state_t state = STATE_DISCOVERING;

typedef enum { PHASE_IDLE, PHASE_AGGREGATING, PHASE_P2P, PHASE_TX } slave_phase_t;
/* Callbacks consult this phase before touching candidate clusters.
 * A late MERGE must not modify a batch after its peer-exchange deadline.
 */
static volatile slave_phase_t current_phase = PHASE_IDLE;
static clock_time_t p2p_cycle_start;
static process_event_t PROCESS_EVENT_AGGREGATE_TICK;

static uint8_t my_level = 255;
static uip_ipaddr_t parent_ip;
static bool is_bound = false;
static int16_t best_score = -10000;
static int16_t current_parent_score = -10000;
static uip_ipaddr_t best_parent_candidate;

static uint8_t parent_load = 0;
static uint8_t current_seq_no = 0;
static clock_time_t last_beacon_time = 0;
/* Set at receipt, not at a particular protothread wait point. */
static bool sync_pending = false;

/* Valid for these short intervals, including clock_time_t wraparound. */
static clock_time_t window_remaining(clock_time_t start, clock_time_t length) {
    clock_time_t elapsed = (clock_time_t)(clock_time() - start);
    return elapsed < length ? (clock_time_t)(length - elapsed) : 0;
}

static struct etimer sync_timer; /* Timer for the local aggregation cycle. */
static uip_ipaddr_t multicast_ip;

/* These candidates are the current snapshot only. Suppression means their
 * contribution was transferred to a peer, so they must not also be sent upward.
 */
static master_payload_t candidate_clusters[MAX_CANDIDATE_CLUSTERS];
static bool cluster_suppressed[MAX_CANDIDATE_CLUSTERS];
static uint8_t num_candidates = 0;
static uint16_t p2p_window_token;

/* Freeze each outgoing contribution for at most one repeat on a matching SNOOP.
 * Never fall back upwards after transfer: acceptance may have happened already. */
#define P2P_MERGE_MIN_REMAINING (CLOCK_SECOND / 2)
#define P2P_MERGE_MAX_SENDS 2
static peer_message_t outgoing_merge[MAX_CANDIDATE_CLUSTERS];
static uip_ipaddr_t outgoing_merge_ip[MAX_CANDIDATE_CLUSTERS];
static uint8_t outgoing_merge_sends[MAX_CANDIDATE_CLUSTERS];

/* Record peer and source-cluster pairs already fused into each candidate. */
static uint16_t merged_peers[MAX_CANDIDATE_CLUSTERS][MAX_PEERS_PER_CLUSTER];
static uint8_t merged_peers_count[MAX_CANDIDATE_CLUSTERS];
static uint8_t merged_source_clusters[MAX_CANDIDATE_CLUSTERS][MAX_PEERS_PER_CLUSTER];

typedef struct data_item {
    struct data_item *next;
    uint8_t type;
    union {
        data_payload_t raw;
        master_payload_t processed;
    } payload;
} data_item_t;

LIST(active_queue);     /* UDP callbacks append incoming records here. */
LIST(processing_queue); /* The processing thread drains one detached snapshot. */
MEMB(data_memb, data_item_t, MAX_QUEUE_ITEMS);

static process_event_t PROCESS_EVENT_SYNC_BEACON;

/* Connection control only: DATA transmission remains fire-and-forget. */
static bool join_multicast = false;
static unsigned long last_parent_contact_seconds;
#define JOIN_MAX_ATTEMPTS 3
#define JOIN_RETRY_INTERVAL CLOCK_SECOND

PROCESS(slave_node_process, "Slave Networking");
PROCESS(processing_process, "Data Fusion Processing");
PROCESS(cleanup_process, "Child Cleanup");
PROCESS(keepalive_process, "Slave Keepalive");

AUTOSTART_PROCESSES(&slave_node_process, &processing_process, &cleanup_process, &keepalive_process);

/* Only active entries contribute to the advertised child count. Expired slots
 * remain allocated in the fixed array and can later be reused.
 */
static uint8_t get_active_children_count() {
    uint8_t count = 0;
    for (int i = 0; i < MAX_CHILDREN; i++) {
        if (connected_children[i].is_active) {
            count++;
        }
    }
    return count;
}

/* Look up an application ID, then reuse an inactive table entry if necessary.
 * A full table returns NULL; callers must not dereference that result.
 */
child_state_t *get_child_state(uint16_t id) {
    for (int i = 0; i < MAX_CHILDREN; i++) {
        if (connected_children[i].is_active && connected_children[i].node_id == id) {
            return &connected_children[i];
        }
    }
    for (int i = 0; i < MAX_CHILDREN; i++) {
        if (!connected_children[i].is_active) {
            connected_children[i].is_active = true;
            connected_children[i].node_id = id;
            connected_children[i].last_seen = clock_seconds();
            connected_children[i].first_packet_received = false;
            connected_children[i].last_seq_no = 0;
            return &connected_children[i];
        }
    }
    return NULL;
}

/* Keep receive filtering open after sending to a specific destination. */

/* Sending is asynchronous from the application perspective: this call does not
 * wait for a MAC acknowledgement and its return is not a delivery receipt.
 */
static void safe_udp_send(struct simple_udp_connection *c, const void *data, uint16_t datalen,
                          const uip_ipaddr_t *to) {

    simple_udp_sendto(c, data, datalen, to);

    /* Restore the wildcard remote address so subsequent senders remain eligible. */
    if (c != NULL && c->udp_conn != NULL) {
        uip_create_unspecified(&c->udp_conn->ripaddr);
    }
}

/* Handle same-level SNOOP advertisements and targeted MERGE contributions. */
static void rx_peer_callback(struct simple_udp_connection *c, const uip_ipaddr_t *sender_addr,
                             uint16_t sender_port, const uip_ipaddr_t *receiver_addr,
                             uint16_t receiver_port, const uint8_t *data, uint16_t datalen) {

    if (!is_bound || datalen < sizeof(peer_message_t)) {
        return;
    }
    /* UDP output can reuse the input buffer: preserve packet and address. */
    static peer_message_t incoming_peer;
    static uip_ipaddr_t incoming_peer_ip;
    memcpy(&incoming_peer, data, sizeof(incoming_peer));
    uip_ipaddr_copy(&incoming_peer_ip, sender_addr);
    const peer_message_t *peer_msg = &incoming_peer;
    if (peer_msg->aggregated_nodes_count == 0 ||
        peer_msg->source_cluster_id >= MAX_CANDIDATE_CLUSTERS) {
        return;
    }
    uint16_t peer_id = uip_ntohs(peer_msg->slave_id);
    if (peer_id == my_slave_id) {
        return;
    }

    /* Peers need the same application level. This check does not require that
     * they have the same parent, and does not establish global loop freedom.
     */
    if (peer_msg->level != my_level) {
        return; /* Only same-level peers may participate in horizontal fusion. */
    }

    if (num_candidates == 0 || current_phase != PHASE_P2P ||
        window_remaining(p2p_cycle_start, AGGREGATION_WINDOW + PEER_WAIT_WINDOW) == 0) {
        if (peer_msg->msg_type == MSG_TYPE_PEER_MERGE &&
            uip_ntohs(peer_msg->target_id) == my_slave_id) {
            LOG_WARN("P2P MERGE rejected phase=%u candidates=%u remaining=%lu\n", current_phase,
                     num_candidates,
                     (unsigned long)window_remaining(p2p_cycle_start,
                                                     AGGREGATION_WINDOW + PEER_WAIT_WINDOW));
        }
        return;
    }

#if !PIPE_COMPACT_LOG
    LOG_INFO("P2P RX CALLBACK FIRED from %u, msg_type=%u, phase=%u\n", peer_id, peer_msg->msg_type,
             current_phase);
#endif

    static uint16_t peer_mags[FFT_SAMPLES];
    static uint16_t my_mags[FFT_SAMPLES];
    static peer_message_t merge_msg;

    uint8_t dimension_count = (USE_CSV_TRACES == 1) ? 1 : FFT_SAMPLES;
    for (int i = 0; i < FFT_SAMPLES; i++) {
        peer_mags[i] = uip_ntohs(peer_msg->magnitudes_net[i]);
    }

    if (peer_msg->msg_type == MSG_TYPE_PEER_SNOOP) {
#if !PIPE_COMPACT_LOG
        LOG_INFO("P2P: Heard SNOOP from Peer %u for Cluster %u\n", peer_id, peer_msg->cluster_id);
#endif
        for (uint8_t i = 0; i < num_candidates; i++) {
            if (cluster_suppressed[i]) {
                /* Repeat only the exact frozen transfer, never a new target. */
                if (outgoing_merge_sends[i] > 0 && outgoing_merge_sends[i] < P2P_MERGE_MAX_SENDS &&
                    outgoing_merge[i].target_id == peer_msg->slave_id &&
                    outgoing_merge[i].cluster_id == peer_msg->cluster_id &&
                    outgoing_merge[i].window_token_net == peer_msg->window_token_net &&
                    uip_ipaddr_cmp(&outgoing_merge_ip[i], &incoming_peer_ip) &&
                    window_remaining(p2p_cycle_start, AGGREGATION_WINDOW + PEER_WAIT_WINDOW) >
                        P2P_MERGE_MIN_REMAINING) {
                    outgoing_merge_sends[i]++;
                    LOG_INFO("P2P MERGE repeat src=%u dst=%u token=%u\n", i, peer_id,
                             uip_ntohs(peer_msg->window_token_net));
                    safe_udp_send(&udp_peer_conn, &outgoing_merge[i], sizeof(peer_message_t),
                                  &outgoing_merge_ip[i]);
                }
                continue;
            }

            for (int k = 0; k < FFT_SAMPLES; k++) {
                my_mags[k] = uip_ntohs(candidate_clusters[i].final_magnitudes_net[k]);
            }

            if (is_spatially_similar(my_mags, peer_mags, SPATIAL_DEDUP_THRESHOLD_SQ,
                                     dimension_count)) {
                /* Resolve symmetric similarity decisions using application IDs. The higher
                 * ID contributes to the lower ID, avoiding mutual suppression for this pair.
                 */
                if (my_slave_id > peer_id) {
                    /* If no transfer has started, retain the normal upward path
                     * when too little time remains for MAC queueing/retries. */
                    if (window_remaining(p2p_cycle_start, AGGREGATION_WINDOW + PEER_WAIT_WINDOW) <=
                        P2P_MERGE_MIN_REMAINING) {
                        LOG_INFO("P2P late SNOOP: keep upward src=%u\n", i);
                        continue;
                    }
                    /* Suppress before sending the MERGE. There is no application acceptance ACK;
                     * blind upward fallback could duplicate a contribution already accepted by the
                     * peer.
                     */
                    cluster_suppressed[i] = true;
                    merge_msg.msg_type = MSG_TYPE_PEER_MERGE;
                    merge_msg.slave_id = uip_htons(my_slave_id);

                    /* Address the contribution to the peer whose SNOOP selected this window. */
                    merge_msg.target_id = uip_htons(peer_id);

                    merge_msg.level = my_level;
                    merge_msg.window_token_net = peer_msg->window_token_net;
                    merge_msg.source_cluster_id = i;
                    merge_msg.cluster_id = peer_msg->cluster_id;
                    merge_msg.aggregated_nodes_count = candidate_clusters[i].aggregated_nodes_count;
                    for (int k = 0; k < FFT_SAMPLES; k++) {
                        merge_msg.magnitudes_net[k] = uip_htons(my_mags[k]);
                    }

                    LOG_INFO("P2P: Snooped similar Event. Suppressing my Cluster %u. Sending "
                             "MERGE to Peer %u (Adds %u nodes).\n",
                             i, peer_id, merge_msg.aggregated_nodes_count);

                    /* Keep the exact payload, destination and target-window token for a bounded
                     * repeat. A retry must not include a later change in the candidate weight.
                     */
                    outgoing_merge[i] = merge_msg;
                    uip_ipaddr_copy(&outgoing_merge_ip[i], &incoming_peer_ip);
                    outgoing_merge_sends[i] = 1;
                    LOG_INFO("P2P MERGE unicast src=%u dst=%u token=%u\n", i, peer_id,
                             uip_ntohs(merge_msg.window_token_net));
                    safe_udp_send(&udp_peer_conn, &outgoing_merge[i], sizeof(peer_message_t),
                                  &outgoing_merge_ip[i]);
                }
            }
        }
    } else if (peer_msg->msg_type == MSG_TYPE_PEER_MERGE) {
        /* Ignore MERGE traffic addressed to another application ID. */
        if (uip_ntohs(peer_msg->target_id) != my_slave_id) {
            return;
        }

        if (uip_ntohs(peer_msg->window_token_net) != p2p_window_token) {
            LOG_WARN("P2P stale MERGE dropped\n");
            return;
        }
        /* The target cluster number belongs to our current window. It differs from
         * source_cluster_id, which identifies the sender contribution in the ledger.
         */
        uint8_t c_id = peer_msg->cluster_id;
        if (c_id < num_candidates && !cluster_suppressed[c_id]) {

            /* A repeated contribution must not increase the cluster weight twice. */
            bool already_merged = false;
            for (int m = 0; m < merged_peers_count[c_id]; m++) {
                if (merged_peers[c_id][m] == peer_id &&
                    merged_source_clusters[c_id][m] == peer_msg->source_cluster_id) {
                    already_merged = true;
                    break;
                }
            }

            if (!already_merged) {
                /* Never fuse an unrecordable contribution: retries must be idempotent. */
                if (merged_peers_count[c_id] >= MAX_PEERS_PER_CLUSTER) {
                    LOG_WARN("P2P merge ledger full; contribution dropped\n");
                    return;
                }
                uint16_t total_weight = (uint16_t)candidate_clusters[c_id].aggregated_nodes_count +
                                        peer_msg->aggregated_nodes_count;
                if (total_weight == 0 || total_weight > 255) {
                    LOG_WARN("P2P weight overflow; contribution dropped\n");
                    return;
                }
                /* Reserve a ledger entry before increasing the weight. Each recorded pair
                 * is accepted once in this window, including when its packet is repeated.
                 */
                uint8_t slot = merged_peers_count[c_id]++;
                merged_peers[c_id][slot] = peer_id;
                merged_source_clusters[c_id][slot] = peer_msg->source_cluster_id;
                LOG_INFO("P2P: Received MERGE from Peer %u. Fusing %u nodes to my Cluster %u!\n",
                         peer_id, peer_msg->aggregated_nodes_count, c_id);
                for (int k = 0; k < FFT_SAMPLES; k++) {
                    my_mags[k] = uip_ntohs(candidate_clusters[c_id].final_magnitudes_net[k]);
                }

                for (int k = 0; k < dimension_count; k++) {
                    /* Multiply in a wider type before division, then truncate the weighted mean.
                     * The payload stores hundredths, so no floating-point operations are needed.
                     */
                    uint32_t new_val =
                        (((uint32_t)my_mags[k] * candidate_clusters[c_id].aggregated_nodes_count) +
                         ((uint32_t)peer_mags[k] * peer_msg->aggregated_nodes_count)) /
                        total_weight;
                    candidate_clusters[c_id].final_magnitudes_net[k] = uip_htons((uint16_t)new_val);
                }
                candidate_clusters[c_id].aggregated_nodes_count = total_weight;
            }
        }
    }
}

/* Handle parent discovery, binding, migration and phase updates. */
static void rx_from_parent_callback(struct simple_udp_connection *c,
                                    const uip_ipaddr_t *sender_addr, uint16_t sender_port,
                                    const uip_ipaddr_t *receiver_addr, uint16_t receiver_port,
                                    const uint8_t *data, uint16_t datalen) {
    if (datalen < sizeof(control_message_t)) {
        return;
    }
    /* The first byte selects the packed payload layout. Length checks protect the
     * subsequent casts; multibyte fields are converted when used on the host.
     */
    uint8_t type = data[0];

    if (type == MSG_TYPE_BEACON) {
        control_message_t *msg = (control_message_t *)data;
        /* A bound relay considers only advertisements from a lower level as parents.
         * Same-level traffic belongs to peer exchange, not upward attachment.
         */
        if (msg->level >= my_level && state == STATE_BOUND) {
            return;
        }

        int16_t rssi = packetbuf_attr(PACKETBUF_ATTR_RSSI);
        /* Prefer strong links, but penalize a parent already carrying more load.
         * Hysteresis below prevents switching for every small score improvement.
         */
        int16_t score = rssi - (msg->load * LOAD_PENALTY);

        /* During discovery, remember the best advertisement. Do not send a JOIN from
         * the receive callback; the networking process owns that transaction.
         */
        if (state == STATE_DISCOVERING) {
#if !PIPE_COMPACT_LOG
            LOG_INFO("Heard BEACON Lvl %u from %u (Score %d)\n", msg->level,
                     uip_ntohs(msg->node_id), score);
#endif
            if (score > best_score) {
                best_score = score;
                uip_ipaddr_copy(&best_parent_candidate, sender_addr);
            }
        } else if (state == STATE_BOUND) {
            if (uip_ipaddr_cmp(sender_addr, &parent_ip)) {
                current_parent_score = score;
                /* Record receipt time independently of event delivery. A missed wakeup must
                 * not make a recently heard parent appear dead.
                 */
                last_parent_contact_seconds = clock_seconds();
                parent_load = msg->load;
                if (pipeline_phase_decode(msg, &last_beacon_time)) {
                    sync_pending = true;
                }

                /* Wake the processing thread after recording the parent timing reference. */
                process_post(&processing_process, PROCESS_EVENT_SYNC_BEACON, NULL);
                process_post(&keepalive_process, PROCESS_EVENT_MSG, NULL);
            } else if (score > current_parent_score + HYSTERESIS_MARGIN && msg->level < my_level) {
                pipeline_schedule_reset_eb();
                LOG_INFO("Migration! Better Parent %u found (Score %d vs %d)\n",
                         uip_ntohs(msg->node_id), score, current_parent_score);
                uip_ipaddr_copy(&best_parent_candidate, sender_addr);
                best_score = score;
                sync_pending = false;
                num_candidates = 0; /* Abort old topology P2P batch; keep active_queue. */
                is_bound = false;
                my_level = 255;
                state = STATE_WAITING_OFFER;

                join_multicast = false;
                process_poll(&slave_node_process);
            }
        }
    } else if (type == MSG_TYPE_OFFER && state == STATE_WAITING_OFFER) {
        /* A directed JOIN accepts an OFFER only from its selected candidate.
         * A multicast probe has no preselected responder.
         */
        if (!join_multicast && !uip_ipaddr_cmp(sender_addr, &best_parent_candidate)) {
            LOG_WARN("JOIN ignoring OFFER from non-selected parent\n");
            return;
        }
        last_parent_contact_seconds = clock_seconds();
        control_message_t *msg = (control_message_t *)data;
        uip_ipaddr_copy(&parent_ip, sender_addr);
        /* Derive the application level from the accepted parent. This level is not
         * the TSCH join priority or the number of radio hops to the time source.
         */
        my_level = msg->level + 1;
        current_parent_score = best_score;
        parent_load = msg->load;
        sync_pending = false; /* OFFER is not a phase reference. */
        is_bound = true;
        state = STATE_BOUND;
        pipeline_schedule_reset_eb();
        LOG_INFO("Bound to Parent %u. I am now LEVEL %u\n", uip_ntohs(msg->node_id), my_level);

        process_post(&processing_process, PROCESS_EVENT_SYNC_BEACON, NULL);
        process_poll(&keepalive_process);
        process_poll(&slave_node_process);
    }
}

/* Handle JOIN requests and input from children. */
static void rx_from_children_callback(struct simple_udp_connection *c,
                                      const uip_ipaddr_t *sender_addr, uint16_t sender_port,
                                      const uip_ipaddr_t *receiver_addr, uint16_t receiver_port,
                                      const uint8_t *data, uint16_t datalen) {
    if (!is_bound) {
        return;
    }
    if (datalen < 1) {
        return;
    }
    uint8_t type = data[0];
    if (type == MSG_TYPE_JOIN && datalen < sizeof(control_message_t)) {
        return;
    }
    /* DATA and processed DATA have different lengths. Do not read either payload
     * until its complete fixed-size structure is available.
     */
    if (type == MSG_TYPE_DATA && datalen < sizeof(data_payload_t)) {
        return;
    }
    if (type == MSG_TYPE_PROCESSED_DATA && datalen < sizeof(master_payload_t)) {
        return;
    }

    if (type == MSG_TYPE_JOIN) {
        pipeline_schedule_reset_eb();
        control_message_t *msg = (control_message_t *)data;
        uint16_t child_id = uip_ntohs(msg->node_id);
#if !PIPE_COMPACT_LOG
        LOG_INFO("Received JOIN request from Node %u. Sending OFFER...\n", child_id);
#endif

        child_state_t *child_state = get_child_state(child_id);
        if (child_state != NULL) {
            child_state->last_seen = clock_seconds();
        }

        control_message_t offer_msg;
        offer_msg.msg_type = MSG_TYPE_OFFER;
        offer_msg.node_id = uip_htons(my_slave_id);
        offer_msg.level = my_level;
        /* Advertise the local child count plus the remembered parent load. This is
         * a selection heuristic, not a globally exact count of all descendants.
         */
        offer_msg.load = get_active_children_count() + parent_load;
        offer_msg.phase_offset_net = 0;
        offer_msg.phase_sent_net = 0;
        safe_udp_send(&udp_server_conn, &offer_msg, sizeof(offer_msg), sender_addr);

    } else if (type == MSG_TYPE_DATA || type == MSG_TYPE_PROCESSED_DATA) {
        uint8_t incoming_seq = (type == MSG_TYPE_DATA) ? ((data_payload_t *)data)->seq_no
                                                       : ((master_payload_t *)data)->seq_no;
        uint16_t child_id = (type == MSG_TYPE_DATA)
                                ? uip_ntohs(((data_payload_t *)data)->node_id)
                                : uip_ntohs(((master_payload_t *)data)->slave_id);

        child_state_t *child_state = get_child_state(child_id);

        /* Reject an exact queued duplicate before allocating another pool entry. */
        bool is_duplicate = false;
        for (data_item_t *iter = list_head(active_queue); iter != NULL;
             iter = list_item_next(iter)) {
            uint16_t iter_id = (iter->type == MSG_TYPE_DATA)
                                   ? uip_ntohs(iter->payload.raw.node_id)
                                   : uip_ntohs(iter->payload.processed.slave_id);
            uint8_t iter_seq = (iter->type == MSG_TYPE_DATA) ? iter->payload.raw.seq_no
                                                             : iter->payload.processed.seq_no;
            uint8_t iter_cluster =
                (iter->type == MSG_TYPE_DATA) ? 0 : iter->payload.processed.cluster_id;
            uint8_t inc_cluster =
                (type == MSG_TYPE_DATA) ? 0 : ((master_payload_t *)data)->cluster_id;

            if (iter_id == child_id && iter_seq == incoming_seq && iter_cluster == inc_cluster) {
                is_duplicate = true;
                break;
            }
        }

        if (!is_duplicate) {
            /* The pool is shared by active and processing lists. Allocation failure drops
             * this input; increasing its capacity also increases static RAM usage.
             */
            data_item_t *item = memb_alloc(&data_memb);
            if (item != NULL) {
#if PIPE_DIAGNOSTICS
                LOG_INFO("PIPE RX src=%u seq=%u cluster=%u weight=%u type=%u now=%lu\n", child_id,
                         incoming_seq,
                         type == MSG_TYPE_DATA ? 0 : ((master_payload_t *)data)->cluster_id,
                         type == MSG_TYPE_DATA ? 1
                                               : ((master_payload_t *)data)->aggregated_nodes_count,
                         type, (unsigned long)clock_time());
#endif
                item->type = type;
                if (type == MSG_TYPE_DATA) {
                    memcpy(&item->payload.raw, data, sizeof(data_payload_t));
                } else {
                    memcpy(&item->payload.processed, data, sizeof(master_payload_t));
                }
                list_add(active_queue, item); /* Leave processing to the next snapshot. */
            } else {
                LOG_WARN("Queue Full! Dropping Seq: %u\n", incoming_seq);
                return;
            }
        }

        if (child_state != NULL) {
            child_state->last_seq_no = incoming_seq;
            child_state->first_packet_received = true;
            child_state->last_seen = clock_seconds();
        }
    }
}

/* Coalesce by source and cluster before calculating magnitudes. Two different
 * sampling cycles can reach this snapshot; only their newer sequence survives. */
/* This function runs without a protothread yield. Every popped pool entry is
 * freed after conversion, replacement or rejection so the next batch can reuse it.
 */
static void drain_queue_to_array(cluster_point_t *collected_points, uint8_t *point_count) {
    data_item_t *item;

    while ((item = list_pop(processing_queue)) != NULL) {
        uint16_t item_origin_id = (item->type == MSG_TYPE_DATA)
                                      ? uip_ntohs(item->payload.raw.node_id)
                                      : uip_ntohs(item->payload.processed.slave_id);
        uint8_t item_cluster_id =
            (item->type == MSG_TYPE_DATA) ? 0 : item->payload.processed.cluster_id;
        uint8_t item_seq_no = (item->type == MSG_TYPE_DATA) ? item->payload.raw.seq_no
                                                            : item->payload.processed.seq_no;

        /* A snapshot has one slot per source and cluster, not per sample epoch.
         * Look for an existing slot before consuming another array entry.
         */
        bool found = false;
        uint8_t target_idx = *point_count;

        for (uint8_t i = 0; i < *point_count; i++) {
            if (collected_points[i].origin_id == item_origin_id &&
                collected_points[i].cluster_id == item_cluster_id) {
                found = true;
                target_idx = i;
                break;
            }
        }

        if (found) {
#if PIPE_DIAGNOSTICS
            LOG_INFO("PIPE COALESCE src=%u cluster=%u seen=%u incoming=%u now=%lu\n",
                     item_origin_id, item_cluster_id, collected_points[target_idx].seq_no,
                     item_seq_no, (unsigned long)clock_time());
#endif
            /* Interpret the eight-bit sequence difference over a short history, including
             * wrap from 255 to 0. Equal or older records do not replace the current value.
             * This comparison is ambiguous for gaps of 128 or more sequence steps.
             */
            if ((int8_t)(item_seq_no - collected_points[target_idx].seq_no) <= 0) {
                memb_free(&data_memb, item);
                continue;
            }
        } else {
            if (*point_count < MAX_QUEUE_ITEMS) {
                (*point_count)++;
            } else {
                LOG_WARN("Points array full! Dropping %u\n", item_origin_id);
                memb_free(&data_memb, item);
                continue;
            }
        }

        collected_points[target_idx].origin_id = item_origin_id;
        collected_points[target_idx].cluster_id = item_cluster_id;
        collected_points[target_idx].seq_no = item_seq_no;
        collected_points[target_idx].visited = false;
        collected_points[target_idx].clustered = false;

        /* Raw sensor input has weight one. CSV mode uses its scalar value directly;
         * signal mode transforms the eight raw samples before clustering.
         */
        if (item->type == MSG_TYPE_DATA) {
            collected_points[target_idx].weight = 1;
            if (USE_CSV_TRACES == 1) {
                collected_points[target_idx].magnitudes[0] =
                    uip_ntohs(item->payload.raw.raw_data_net[0]);
                for (int i = 1; i < FFT_SAMPLES; i++) {
                    collected_points[target_idx].magnitudes[i] = 0;
                }
            } else {
                int16_t host_raw_data[FFT_SAMPLES];
                for (int i = 0; i < FFT_SAMPLES; i++) {
                    host_raw_data[i] = (int16_t)uip_ntohs(item->payload.raw.raw_data_net[i]);
                }
                compute_fft_magnitudes(host_raw_data, collected_points[target_idx].magnitudes);
            }
        } else if (item->type == MSG_TYPE_PROCESSED_DATA) {
            /* Processed input already represents a weighted cluster. Preserve its count
             * and magnitudes instead of applying another signal transform.
             */
            collected_points[target_idx].weight = item->payload.processed.aggregated_nodes_count;
            for (int i = 0; i < FFT_SAMPLES; i++) {
                collected_points[target_idx].magnitudes[i] =
                    uip_ntohs(item->payload.processed.final_magnitudes_net[i]);
            }
        }

        memb_free(&data_memb, item);
    }
}

/* Separate timed collection and peer exchange from connection management. */
PROCESS_THREAD(processing_process, ev, data) {
    static struct etimer agg_timer;
#if PIPE_EXTENDED_TIMING && PIPE_DYNAMIC_SCHEDULE
    static struct etimer uplink_jitter_timer;
    static clock_time_t uplink_jitter;
#endif
    static struct etimer p2p_timer;
    static cluster_point_t collected_points[MAX_QUEUE_ITEMS];
    static uint8_t point_count = 0;
    static uint8_t c;
    static clock_time_t p2p_jitter;

    static clock_time_t cycle_start;
    static clock_time_t remaining;
    static uint8_t strobe;
    static uint8_t tick_counter = 0;
    static bool first_run = true;
    static struct etimer beacon_timer;
    static uint8_t active_count;

    static uint8_t previous_beacon_interval =
        1; /* Start with a beacon every cycle while waiting for children. */

    static control_message_t beacon;

    /* Protothreads resume inside this function after a wait. State needed across
     * a yield must live in static storage, not an ordinary automatic local.
     */
    PROCESS_BEGIN();
    list_init(active_queue);
    list_init(processing_queue);
    memb_init(&data_memb);

    etimer_set(&beacon_timer, CLOCK_SECOND / 2);
    while (uip_ds6_get_link_local(ADDR_PREFERRED) == NULL) {
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&beacon_timer));
        etimer_reset(&beacon_timer);
    }

    etimer_set(&sync_timer, AGGREGATION_TICK);

    while (1) {
        current_phase = PHASE_IDLE;

        /* Wait for the timer state; beacon events only latch a correction. */
        PROCESS_WAIT_UNTIL(etimer_expired(&sync_timer));
        /* Use the scheduled timer expiration as the cycle origin. Using the current
         * time here would accumulate processing and logging delays from cycle to cycle.
         */
        cycle_start = etimer_expiration_time(&sync_timer);
        /* Skip obsolete cycles, never execute a catch-up burst. */
        while (window_remaining(cycle_start, AGGREGATION_TICK) == 0) {
            cycle_start = (clock_time_t)(cycle_start + AGGREGATION_TICK);
            LOG_WARN("PIPE skipped overdue cycle\n");
        }
        etimer_set(&sync_timer, window_remaining(cycle_start, AGGREGATION_TICK));
        LOG_INFO("PIPE cycle start=%lu now=%lu bound=%u\n", (unsigned long)cycle_start,
                 (unsigned long)clock_time(), is_bound);

        if (!is_bound) {
            continue;
        }

        /* Give the next peer window a new token so delayed MERGEs from an earlier
         * window cannot be applied to the current candidates.
         */
        p2p_window_token++;
        current_phase = PHASE_AGGREGATING;
#if !PIPE_COMPACT_LOG
        LOG_INFO("--- PHASE 1: VERTICAL AGGREGATION WINDOW STARTED ---\n");
#endif

        tick_counter++;
        active_count = get_active_children_count();
        uint8_t beacon_interval =
            (active_count >= CHILDREN_THRESHOLD) ? (BEACON_INTERVAL_SLOW / AGGREGATION_TICK) : 1;

        /* Adjust application beacon frequency when the child count changes. */
        if (previous_beacon_interval == 1 && beacon_interval > 1) {
            /* On entering slow beacon mode, choose a counter offset to spread transmissions
             * across cycles. This does not guarantee collision avoidance. */
            tick_counter = random_rand() % beacon_interval;
#if !PIPE_COMPACT_LOG
            LOG_INFO("Network steady. Transition to SLOW Beacons with Phase Shift: %u\n",
                     tick_counter);
#endif
        } else if (previous_beacon_interval > 1 && beacon_interval == 1) {
            /* When the last child expires, advertise promptly to help new nodes discover us.
             */
            tick_counter = beacon_interval;
#if !PIPE_COMPACT_LOG
            LOG_INFO("Child lost. Transition to FAST Beacons.\n");
#endif
        }
        previous_beacon_interval = beacon_interval; /* Remember the interval so the next cycle can
                                                       detect another transition. */

        /* Reset the counter and send the next application beacon when due. */
        if (first_run || tick_counter >= beacon_interval) {
            tick_counter = 0;
            first_run = false;

            /* Add up to roughly 200 ms of jitter among nodes sharing a cycle boundary. */
            clock_time_t jitter = random_rand() % (CLOCK_SECOND / 5);
            if (jitter > 0) {
                etimer_set(&beacon_timer, jitter);
                /* Beacon jitter stays inside the absolute aggregation window. */
                PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&beacon_timer));
            }

            beacon.msg_type = MSG_TYPE_BEACON;
            beacon.node_id = uip_htons(my_slave_id);
            beacon.level = my_level;
            beacon.load = active_count + parent_load;
            LOG_INFO("Broadcasting Slave BEACON (Level %u, Load %u)...\n", my_level, beacon.load);
            /* Carry both the local phase offset and TSCH enqueue time. The receiver can
             * subtract packet age when recovering the parent cycle reference.
             */
            pipeline_phase_stamp(&beacon, cycle_start);
            safe_udp_send(&udp_server_conn, &beacon, sizeof(beacon), &multicast_ip);
        }

        /* Collect vertical input until the snapshot deadline. */
        num_candidates = 0;
        point_count = 0;
        memset(collected_points, 0, sizeof(collected_points));
        memset(merged_peers_count, 0, sizeof(merged_peers_count));
        memset(outgoing_merge_sends, 0, sizeof(outgoing_merge_sends));

        remaining = window_remaining(cycle_start, AGGREGATION_WINDOW);
        if (remaining > 0) {
            etimer_set(&agg_timer, remaining);
            while (window_remaining(cycle_start, AGGREGATION_WINDOW) > 0) {
                PROCESS_WAIT_EVENT();
            }
        }
        LOG_INFO("PIPE snapshot now=%lu\n", (unsigned long)clock_time());

        /* Move the current batch without yielding; later arrivals stay in active_queue. */
        data_item_t *swap_item;
        /* Detach every record currently available at this snapshot. There is no
         * yield inside this transfer; records received afterwards belong to a later batch.
         */
        while ((swap_item = list_pop(active_queue)) != NULL) {
            list_add(processing_queue, swap_item);
        }

        /* Convert and coalesce only the detached snapshot. */
        drain_queue_to_array(collected_points, &point_count);
#if PIPE_DIAGNOSTICS
        {
            uint16_t weight = 0;
            uint8_t j;
            for (j = 0; j < point_count; j++) {
                weight += collected_points[j].weight;
            }
            LOG_INFO("PIPE SNAP points=%u weight=%u now=%lu\n", point_count, weight,
                     (unsigned long)clock_time());
        }
#endif

        if (point_count > 0) {
            run_clustering(collected_points, point_count, candidate_clusters, &num_candidates,
                           my_slave_id);
            for (c = 0; c < num_candidates; c++) {
                cluster_suppressed[c] = false;
            }
        }

        /* Empty windows still traverse P2P/TX and the common sync epilogue. */

        /* Exchange cluster candidates with same-level peers. */
        p2p_cycle_start = cycle_start;
        current_phase = PHASE_P2P;
#if !PIPE_COMPACT_LOG
        LOG_INFO("--- PHASE 2: P2P HORIZONTAL SNOOPING WINDOW STARTED ---\n");
#endif

        /* Choose a SNOOP time in the first half of the peer window. */
        p2p_jitter = random_rand() % (PEER_WAIT_WINDOW / 2);
        remaining = window_remaining(cycle_start, AGGREGATION_WINDOW + PEER_WAIT_WINDOW);
        if (p2p_jitter > remaining) {
            p2p_jitter = remaining;
        }
        etimer_set(&p2p_timer, p2p_jitter);

        while (!etimer_expired(&p2p_timer) &&
               window_remaining(cycle_start, AGGREGATION_WINDOW + PEER_WAIT_WINDOW) > 0) {
            PROCESS_WAIT_EVENT();
            /* Parent callback has already latched the timing sample. */
            /* MERGE callbacks may still update candidates while this process waits. */
        }

        /* Send each SNOOP twice to improve the chance that a peer hears it. */
        for (strobe = 0; strobe < 2; strobe++) {
            for (c = 0; c < num_candidates; c++) {
                if (cluster_suppressed[c] || !is_bound ||
                    window_remaining(cycle_start, AGGREGATION_WINDOW + PEER_WAIT_WINDOW) == 0) {
                    continue;
                }

                peer_message_t snoop;
                snoop.msg_type = MSG_TYPE_PEER_SNOOP;
                snoop.target_id = 0;
                snoop.window_token_net = uip_htons(p2p_window_token);
                snoop.source_cluster_id = c;
                snoop.slave_id = uip_htons(my_slave_id);
                snoop.level = my_level;
                snoop.cluster_id = c;
                snoop.aggregated_nodes_count = candidate_clusters[c].aggregated_nodes_count;
                for (int i = 0; i < FFT_SAMPLES; i++) {
                    snoop.magnitudes_net[i] = candidate_clusters[c].final_magnitudes_net[i];
                }

#if !PIPE_COMPACT_LOG
                LOG_INFO("P2P: Snooping my Cluster %u (Strobe %d)\n", c, strobe + 1);
#endif
                safe_udp_send(&udp_peer_conn, &snoop, sizeof(snoop), &multicast_ip);
            }

            /* Separate the two advertisements by approximately 100 ms. */
            if (strobe == 0) {
                remaining = window_remaining(cycle_start, AGGREGATION_WINDOW + PEER_WAIT_WINDOW);
                if (remaining > CLOCK_SECOND / 10) {
                    remaining = CLOCK_SECOND / 10;
                }
                if (remaining > 0) {
                    etimer_set(&p2p_timer, remaining);
                    while (!etimer_expired(&p2p_timer) &&
                           window_remaining(cycle_start, AGGREGATION_WINDOW + PEER_WAIT_WINDOW) >
                               0) {
                        PROCESS_WAIT_EVENT();
                    }
                }
            }
        }

        /* Keep the remaining peer window open for SNOOP and MERGE callbacks. */
        remaining = window_remaining(cycle_start, AGGREGATION_WINDOW + PEER_WAIT_WINDOW);
        if (remaining > 0) {
            etimer_set(&p2p_timer, remaining);
            while (window_remaining(cycle_start, AGGREGATION_WINDOW + PEER_WAIT_WINDOW) > 0) {
                PROCESS_WAIT_EVENT();
            }
        }
        LOG_INFO("PIPE p2p end now=%lu\n", (unsigned long)clock_time());

        /* Send each unsuppressed candidate toward the current parent. */
        current_phase = PHASE_TX;
#if !PIPE_COMPACT_LOG
        LOG_INFO("--- PHASE 3: TRANSMIT UPWARDS ---\n");
#endif
#if PIPE_EXTENDED_TIMING && PIPE_DYNAMIC_SCHEDULE
        /* Spread competing uplinks over one receiver period. The collection
         * and P2P deadlines stay absolute; this is NOT a new periodic timer. */
        if (num_candidates > 0 && is_bound) {
            uplink_jitter =
                random_rand() % ((PIPE_SCHEDULE_LENGTH * 15UL * CLOCK_SECOND) / 1000UL + 1);
            if (uplink_jitter > 0) {
                etimer_set(&uplink_jitter_timer, uplink_jitter);
                PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&uplink_jitter_timer));
            }
        }
#endif
        if (num_candidates > 0) {
            /* Advance the application sequence for a newly generated batch. This byte
             * wraps naturally and is not a globally unique or persistent sample identifier.
             */
            current_seq_no++;
        }
        for (c = 0; c < num_candidates; c++) {
            if (!cluster_suppressed[c] && is_bound) {
                candidate_clusters[c].seq_no = current_seq_no;
                LOG_INFO("Tx CLUSTER ID %u (Nodes: %u, Seq: %u) UPWARDS\n",
                         candidate_clusters[c].cluster_id,
                         candidate_clusters[c].aggregated_nodes_count, current_seq_no);

                safe_udp_send(&udp_client_conn, &candidate_clusters[c], sizeof(master_payload_t),
                              &parent_ip);
            }
        }

        /* Apply the decoded parent cycle reference after processing, including empty
         * cycles. Beacon decoding accounts for queue age using TSCH network time. */
        /* Apply a pending phase adjustment only after collection, peer exchange and
         * uplink work. Even an empty batch must reach this synchronization epilogue.
         */
        if (sync_pending && is_bound) {
            sync_pending = false;
            remaining = AGGREGATION_TICK -
                        ((clock_time_t)(clock_time() - last_beacon_time) % AGGREGATION_TICK);
            etimer_set(&sync_timer, remaining);
            LOG_INFO("PIPE sync next_in=%lu\n", (unsigned long)remaining);
        }
    }
    PROCESS_END();
}

/* Parent liveness is based on received application traffic, not TSCH association. */
/* Check application-parent liveness once per second. TSCH may remain
 * associated even when application beacons from that parent stop arriving.
 */
PROCESS_THREAD(keepalive_process, ev, data) {
    static struct etimer ka_timer;
    PROCESS_BEGIN();
    etimer_set(&ka_timer, CLOCK_SECOND);
    while (1) {
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&ka_timer));
        if (is_bound && (unsigned long)(clock_seconds() - last_parent_contact_seconds) >=
                            (unsigned long)(PARENT_TIMEOUT / CLOCK_SECOND)) {
            pipeline_schedule_reset_eb();
            LOG_WARN("Parent Timeout! Disconnecting...\n");
            is_bound = false;
            my_level = 255;
            sync_pending = false;
            num_candidates = 0;
            state = STATE_DISCOVERING;
            process_poll(&slave_node_process);
        }
        etimer_set(&ka_timer, CLOCK_SECOND);
    }
    PROCESS_END();
}

/* Expiring children updates advertised load without allocating or freeing a table. */
/* Periodically release stale child-table entries so advertised load can fall.
 * This housekeeping does not delete packets already in the input queues.
 */
PROCESS_THREAD(cleanup_process, ev, data) {
    static struct etimer cleanup_timer;
    PROCESS_BEGIN();
    etimer_set(&cleanup_timer, CLOCK_SECOND * 20);
    while (1) {
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&cleanup_timer));
        unsigned long current_time = clock_seconds();
        for (int i = 0; i < MAX_CHILDREN; i++) {
            if (connected_children[i].is_active &&
                (current_time - connected_children[i].last_seen) > INACTIVE_TIMEOUT) {
                LOG_WARN("Node %u inactive. Removing.\n", connected_children[i].node_id);
                connected_children[i].is_active = false;
            }
        }
        etimer_reset(&cleanup_timer);
    }
    PROCESS_END();
}

/* The networking process handles association and parent transactions.
 * The processing process continues to own the five-second aggregation schedule.
 */
PROCESS_THREAD(slave_node_process, ev, data) {
    static struct etimer check_timer;
    static struct etimer periodic_timer;

    static struct etimer join_timer;
    static clock_time_t join_started;
    static clock_time_t join_remaining;
    static uint8_t join_attempt;
    static struct etimer tsch_join_poll;
    PROCESS_BEGIN();

    /* Joining the TSCH network and selecting an application parent are separate
     * steps. This role is not the MAC coordinator, even when it relays application data.
     */
    tsch_set_coordinator(0);
    if (!pipeline_schedule_init()) {
        PROCESS_EXIT();
    }
    /* Start radio scheduling only after the coordinator setting and local schedule
     * have been configured. Application discovery follows MAC initialization.
     */
    NETSTACK_MAC.on();

    /* Wait for initial MAC association before application discovery. */
    while (!tsch_is_associated) {
        etimer_set(&tsch_join_poll, CLOCK_SECOND / 4);
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&tsch_join_poll));
    }

    etimer_stop(&tsch_join_poll);

    PROCESS_EVENT_SYNC_BEACON = process_alloc_event();

    PROCESS_EVENT_AGGREGATE_TICK = process_alloc_event();

    my_slave_id = mac_to_node_id();
    LOG_INFO("SLAVE started with ID: %u\n", my_slave_id);

    simple_udp_register(&udp_client_conn, UDP_CLIENT_PORT, NULL, UDP_SERVER_PORT,
                        rx_from_parent_callback);
    simple_udp_register(&udp_server_conn, UDP_SERVER_PORT, NULL, UDP_CLIENT_PORT,
                        rx_from_children_callback);
    simple_udp_register(&udp_peer_conn, UDP_PEER_PORT, NULL, UDP_PEER_PORT, rx_peer_callback);

    etimer_set(&check_timer, CLOCK_SECOND / 2);
    while (uip_ds6_get_link_local(ADDR_PREFERRED) == NULL) {
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&check_timer));
        etimer_reset(&check_timer);
    }

    uip_create_linklocal_allnodes_mcast(&multicast_ip);
    state = STATE_DISCOVERING;

    while (1) {
        is_bound = false;
        my_level = 255;

        while (state != STATE_BOUND) {
            if (state == STATE_DISCOVERING) {
#if !PIPE_COMPACT_LOG
                LOG_INFO("Listening for BEACONS from upper levels...\n");
#endif
                best_score = -10000;
                etimer_set(&periodic_timer, CLOCK_SECOND * 10);
                while (state == STATE_DISCOVERING && !etimer_expired(&periodic_timer)) {
                    PROCESS_WAIT_EVENT();
                }

                if (best_score != -10000) {
                    join_multicast = false;
#if !PIPE_COMPACT_LOG
                    LOG_INFO("JOIN selected parent; starting transaction\n");
#endif
                } else {
                    join_multicast = true;
                    LOG_WARN("No parent found; starting multicast JOIN probe\n");
                }
                state = STATE_WAITING_OFFER;
            } else if (state == STATE_WAITING_OFFER) {
                /* A fresh, bounded transaction for discovery AND migration. */
                /* Use one overall OFFER deadline for this transaction. Retries consume the
                 * remaining budget; they do not restart a fresh timeout on every send.
                 */
                join_started = clock_time();
                join_attempt = 0;
                while (state == STATE_WAITING_OFFER &&
                       (clock_time_t)(clock_time() - join_started) < OFFER_TIMEOUT) {
                    if (join_attempt < JOIN_MAX_ATTEMPTS) {
                        control_message_t request = {
                            MSG_TYPE_JOIN, uip_htons(my_slave_id), 0, 0, 0, 0};
                        join_attempt++;
#if !PIPE_COMPACT_LOG
                        LOG_INFO("JOIN attempt=%u multicast=%u\n", join_attempt, join_multicast);
#endif
                        safe_udp_send(&udp_client_conn, &request, sizeof(request),
                                      join_multicast ? &multicast_ip : &best_parent_candidate);
                    }
                    /* Recheck after send/log overhead before unsigned subtraction. */
                    join_remaining = (clock_time_t)(clock_time() - join_started);
                    if (join_remaining >= OFFER_TIMEOUT) {
                        break;
                    }
                    join_remaining = OFFER_TIMEOUT - join_remaining;
                    if (join_attempt < JOIN_MAX_ATTEMPTS && join_remaining > JOIN_RETRY_INTERVAL) {
                        join_remaining = JOIN_RETRY_INTERVAL;
                    }
                    etimer_set(&join_timer, join_remaining);
                    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&join_timer) ||
                                             state != STATE_WAITING_OFFER);
                }
                /* Cancel a timer left over from an accepted OFFER before returning to the
                 * bound state or starting another discovery attempt.
                 */
                etimer_stop(&join_timer);
                if (state == STATE_WAITING_OFFER) {
                    LOG_WARN("JOIN timeout after %u attempts; returning to discovery\n",
                             join_attempt);
                    state = STATE_DISCOVERING;
                }
            }
        }

        while (is_bound) {
            PROCESS_WAIT_EVENT_UNTIL(!is_bound);
        }
    }
    PROCESS_END();
}
