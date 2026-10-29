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

#define LOG_MODULE "MasterApp"
#define LOG_LEVEL LOG_LEVEL_INFO

#ifndef USE_CSV_TRACES
#    define USE_CSV_TRACES 1
#endif

/* Each connection binds a local/remote port pair. A wildcard remote IP allows
 * messages from more than one neighbor; the application decides which sender to accept.
 */
static struct simple_udp_connection udp_server_conn;
/* This is the MAC-derived application ID, not the numeric Cooja mote label.
 */
static uint16_t my_master_id;

typedef struct {
    bool is_active;
    uint16_t node_id;
    unsigned long last_seen;
    bool first_packet_received;
    uint8_t last_seq_no;
} child_state_t;

static child_state_t connected_children[MAX_CHILDREN];

typedef struct data_item {
    struct data_item *next;
    uint8_t type;
    union {
        data_payload_t raw;
        master_payload_t processed;
    } payload;
} data_item_t;

/* Both queues draw from the same fixed MEMB pool. The processing list holds
 * only the batch detached at the current report deadline.
 */
LIST(active_queue);
LIST(processing_queue);
MEMB(data_memb, data_item_t, MAX_QUEUE_ITEMS);

static master_payload_t candidate_clusters[MAX_CANDIDATE_CLUSTERS];
static uint8_t num_candidates = 0;

PROCESS(master_node_process, "Master Node Process");
PROCESS(processing_process, "Master Aggregation Process");
PROCESS(cleanup_process, "Master Cleanup Process");

AUTOSTART_PROCESSES(&master_node_process, &processing_process, &cleanup_process);

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

/* Find an existing child or claim a free entry in the bounded table. */
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

static void udp_rx_callback(struct simple_udp_connection *c, const uip_ipaddr_t *sender_addr,
                            uint16_t sender_port, const uip_ipaddr_t *receiver_addr,
                            uint16_t receiver_port, const uint8_t *data, uint16_t datalen) {
    if (datalen < 1) {
        return;
    }
    /* The first byte selects the packed payload layout. Length checks protect the
     * subsequent casts; multibyte fields are converted when used on the host.
     */
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

    /* The master advertises level zero and offers itself as a parent.
     * Its OFFER establishes connectivity but does not carry a usable phase reference.
     */
    if (type == MSG_TYPE_JOIN) {
        pipeline_schedule_reset_eb();
        control_message_t *msg = (control_message_t *)data;
        uint16_t child_id = uip_ntohs(msg->node_id);
#if !PIPE_COMPACT_LOG
        LOG_INFO("Received JOIN request from Node %u. Sending OFFER (Level 0)...\n", child_id);
#endif

        child_state_t *child_state = get_child_state(child_id);
        if (child_state != NULL) {
            child_state->last_seen = clock_seconds();
        }

        control_message_t offer_msg = {MSG_TYPE_OFFER, uip_htons(my_master_id), 0, 0, 0, 0};
        safe_udp_send(&udp_server_conn, &offer_msg, sizeof(offer_msg), sender_addr);

    } else if (type == MSG_TYPE_DATA || type == MSG_TYPE_PROCESSED_DATA) {
        uint8_t incoming_seq = (type == MSG_TYPE_DATA) ? ((data_payload_t *)data)->seq_no
                                                       : ((master_payload_t *)data)->seq_no;
        uint16_t child_id = (type == MSG_TYPE_DATA)
                                ? uip_ntohs(((data_payload_t *)data)->node_id)
                                : uip_ntohs(((master_payload_t *)data)->slave_id);

        child_state_t *child_state = get_child_state(child_id);

        /* Queue received input here; clustering runs later in the aggregation process. */
        /* Reject exact duplicates still queued for this snapshot, using source, sequence
         * and cluster. */
        bool is_exact_duplicate = false;
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

            /* An identical source/sequence/cluster already waiting in this queue is a
             * duplicate. A different sequence is still admitted and may later be coalesced.
             */
            if (iter_id == child_id && iter_seq == incoming_seq && iter_cluster == inc_cluster) {
                is_exact_duplicate = true;
                break;
            }
        }

        /* Allocate from the bounded pool only after checking for a duplicate. */
        if (!is_exact_duplicate) {
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
                list_add(active_queue, item); /* Callbacks append to the active queue. */
            } else {
                LOG_WARN("Queue Full! Dropping Seq: %u\n", incoming_seq);
                return;
            }
        }

        /* Refresh child liveness after accepting traffic. */
        if (child_state != NULL) {
            child_state->last_seq_no = incoming_seq;
            child_state->first_packet_received = true;
            child_state->last_seen = clock_seconds();
        }
    }
}

/* Keep one record per source/cluster in a snapshot. A later sequence replaces
 * the earlier one; this is a latest-value policy, not lossless epoch storage. */
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

/* Run reports against absolute cycle boundaries rather than chaining relative delays.
 */
/* This process owns the report cadence and expensive aggregation work.
 * The UDP callback only validates and queues input.
 */
PROCESS_THREAD(processing_process, ev, data) {
    static struct etimer sync_timer;
    static struct etimer agg_timer;
    static cluster_point_t collected_points[MAX_QUEUE_ITEMS];
    static uint8_t point_count = 0;
    static clock_time_t cycle_start;
    static unsigned long report_no = 0;
    static uip_ipaddr_t multicast_ip;

    static uint8_t tick_counter = 0;
    static bool first_run = true;
    static struct etimer beacon_timer;
    static uint8_t active_count;

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

    uip_create_linklocal_allnodes_mcast(&multicast_ip);
    etimer_set(&sync_timer, AGGREGATION_TICK);

    while (1) {
        PROCESS_WAIT_UNTIL(etimer_expired(&sync_timer));
        /* Use the scheduled timer expiration as the cycle origin. Using the current
         * time here would accumulate processing and logging delays from cycle to cycle.
         */
        cycle_start = etimer_expiration_time(&sync_timer);
        etimer_reset(&sync_timer);
        /* If execution is more than a full period late, skip obsolete cycles instead
         * of producing a burst of stale snapshots and reports.
         */
        while ((clock_time_t)(clock_time() - cycle_start) >= AGGREGATION_TICK) {
            cycle_start = (clock_time_t)(cycle_start + AGGREGATION_TICK);
            etimer_reset(&sync_timer);
            LOG_WARN("PIPE master skipped overdue cycle\n");
        }
        /* Arm the selected report deadline before spending time on beacon jitter. */
        if ((clock_time_t)(clock_time() - cycle_start) < MASTER_AGGREGATION_WINDOW) {
            etimer_set(&agg_timer,
                       MASTER_AGGREGATION_WINDOW - (clock_time_t)(clock_time() - cycle_start));
        }

        tick_counter++;
        active_count = get_active_children_count();
        uint8_t beacon_interval =
            (active_count >= CHILDREN_THRESHOLD) ? (BEACON_INTERVAL_SLOW / AGGREGATION_TICK) : 1;

        /* Reset the beacon counter when its interval expires. */
        if (first_run || tick_counter >= beacon_interval) {
            tick_counter = 0;
            first_run = false;

            /* Spread application beacons over approximately 200 ms. */
            clock_time_t jitter = random_rand() % (CLOCK_SECOND / 5);
            if (jitter > 0) {
                etimer_set(&beacon_timer, jitter);
                PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&beacon_timer));
            }

            beacon.msg_type = MSG_TYPE_BEACON;
            beacon.node_id = uip_htons(my_master_id);
            beacon.level = 0;
            beacon.load = active_count;
            LOG_INFO("Broadcasting Master BEACON (Level %u, Load %u)...\n", beacon.level,
                     beacon.load);
            /* Carry both the local phase offset and TSCH enqueue time. The receiver can
             * subtract packet age when recovering the parent cycle reference.
             */
            pipeline_phase_stamp(&beacon, cycle_start);
            safe_udp_send(&udp_server_conn, &beacon, sizeof(beacon), &multicast_ip);
        }

        num_candidates = 0;
        point_count = 0;
        memset(collected_points, 0, sizeof(collected_points));

#if !PIPE_COMPACT_LOG
        LOG_INFO("--- MASTER AGGREGATION WINDOW STARTED ---\n");
#endif
        /* The report deadline already includes the time spent waiting to send the beacon.
         */

        while ((clock_time_t)(clock_time() - cycle_start) < MASTER_AGGREGATION_WINDOW) {
            PROCESS_WAIT_EVENT();
        }

        /* Move the current batch without yielding. Later arrivals stay in active_queue. */
        data_item_t *swap_item;
        /* Detach every record currently available at this snapshot. There is no
         * yield inside this transfer; records received afterwards belong to a later batch.
         */
        while ((swap_item = list_pop(active_queue)) != NULL) {
            list_add(processing_queue, swap_item);
        }

        /* Only the detached processing batch contributes to this report. */
        drain_queue_to_array(collected_points, &point_count);

        if (point_count > 0) {
            run_clustering(collected_points, point_count, candidate_clusters, &num_candidates,
                           my_master_id);
        }

        /* A report is emitted even for an empty snapshot. Its event count is the
         * number of output clusters, not a count of globally unique physical events.
         */
        report_no++;
        LOG_INFO("PIPE report=%lu scheduled=%lu actual=%lu points=%u events=%u\n", report_no,
                 (unsigned long)(clock_time_t)(cycle_start + MASTER_AGGREGATION_WINDOW),
                 (unsigned long)clock_time(), point_count, num_candidates);
        {
            LOG_INFO("Report boundary\n");
            LOG_INFO("MASTER FINAL REPORT: Generated %u Unique Event(s)\n", num_candidates);
            for (uint8_t c = 0; c < num_candidates; c++) {
                /* Convert the selected magnitude from wire order before formatting hundredths.
                 * The printed sensor count is aggregate weight, not verified source provenance.
                 */
                uint16_t m0 = uip_ntohs(candidate_clusters[c].final_magnitudes_net[0]);

                if (USE_CSV_TRACES == 1) {
                    LOG_INFO(" -> Event %u: Value = %u.%02u (Aggregated from %u total sensors)\n",
                             c + 1, m0 / 100, m0 % 100,
                             candidate_clusters[c].aggregated_nodes_count);
                } else {
                    LOG_INFO(" -> Event %u: Mag[0] = %u.%02u (Aggregated from %u total sensors)\n",
                             c + 1, m0 / 100, m0 % 100,
                             candidate_clusters[c].aggregated_nodes_count);
                }
            }
            LOG_INFO("Report boundary\n");
        }
    }
    PROCESS_END();
}

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
                connected_children[i].is_active = false;
            }
        }
        etimer_reset(&cleanup_timer);
    }
    PROCESS_END();
}

PROCESS_THREAD(master_node_process, ev, data) {
    static struct etimer check_timer;
    PROCESS_BEGIN();

    /* The master starts the TSCH network. Being the MAC coordinator is separate
     * from its application role as the root collector.
     */
    tsch_set_coordinator(1);
    if (!pipeline_schedule_init()) {
        PROCESS_EXIT();
    }
    /* Start radio scheduling only after the coordinator setting and local schedule
     * have been configured. Application discovery follows MAC initialization.
     */
    NETSTACK_MAC.on();

    my_master_id = mac_to_node_id();
    LOG_INFO("MASTER started with ID: %u\n", my_master_id);

    simple_udp_register(&udp_server_conn, UDP_SERVER_PORT, NULL, UDP_CLIENT_PORT, udp_rx_callback);

    etimer_set(&check_timer, CLOCK_SECOND / 2);
    while (uip_ds6_get_link_local(ADDR_PREFERRED) == NULL) {
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&check_timer));
        etimer_reset(&check_timer);
    }

    LOG_INFO("Master Network Ready (Listening for incoming Data...)\n");
    while (1) {
        PROCESS_WAIT_EVENT();
    }
    PROCESS_END();
}
