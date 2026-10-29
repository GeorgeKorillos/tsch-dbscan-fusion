#include "contiki.h"
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
#include "pipeline_sample_timing.h"
#include "pipeline_schedule.h"
#include "sys/log.h"

#ifndef USE_CSV_TRACES
#    define USE_CSV_TRACES 1
#endif

#ifndef SENSOR_ID_OFFSET
#    define SENSOR_ID_OFFSET 5
#endif

#if USE_CSV_TRACES == 1
#    include "sensor_traces.h"
#endif

#define LOG_MODULE "SensorApp"
#define LOG_LEVEL LOG_LEVEL_INFO

/* Each connection binds a local/remote port pair. A wildcard remote IP allows
 * messages from more than one neighbor; the application decides which sender to accept.
 */
static struct simple_udp_connection udp_conn;
static uip_ipaddr_t bound_slave_ip;
static bool is_bound = false;
static uint16_t my_sensor_id;
/* The table is sensor-major. This index selects a trace; current_trace_row
 * selects the sample within that trace and wraps when the recording ends.
 */
static uint8_t mapped_csv_col = 0;
static uint16_t current_trace_row = 0;

static uint8_t current_seq_no = 0;
/* A received beacon latches a correction for the next timer update.
 * It must not cause an extra DATA transmission from the callback.
 */
static bool phase_pending = false;
static clock_time_t parent_cycle_start;
static clock_time_t sample_phase_offset;
static data_payload_t data_payload;

typedef enum { STATE_DISCOVERING, STATE_WAITING_OFFER, STATE_BOUND } sensor_state_t;
static sensor_state_t state = STATE_DISCOVERING;
static int16_t best_score = -10000;
static int16_t current_parent_score = -10000;
static uip_ipaddr_t best_slave_candidate;

static process_event_t PROCESS_EVENT_SYNC_BEACON;

/* Connection control only: DATA transmission remains fire-and-forget. */
static bool join_multicast = false;
static unsigned long last_parent_contact_seconds;
#define JOIN_MAX_ATTEMPTS 3
#define JOIN_RETRY_INTERVAL CLOCK_SECOND

PROCESS(sensor_node_process, "Sensor Node");
PROCESS(keepalive_process, "Sensor Keepalive");
AUTOSTART_PROCESSES(&sensor_node_process, &keepalive_process);

/* Keep receive filtering open after sending to a specific destination. */

/* Sending is asynchronous from the application perspective: this call does not
 * wait for a MAC acknowledgement and its return is not a delivery receipt.
 */
static void safe_udp_send(struct simple_udp_connection *c, const void *data, uint16_t datalen,
                          const uip_ipaddr_t *to) {

    simple_udp_sendto(c, data, datalen, to);

    /* Restore the wildcard remote address so beacons from other parents remain eligible.
     */
    if (c != NULL && c->udp_conn != NULL) {
        uip_create_unspecified(&c->udp_conn->ripaddr);
    }
}

static void sensor_rx_callback(struct simple_udp_connection *c, const uip_ipaddr_t *sender_addr,
                               uint16_t sender_port, const uip_ipaddr_t *receiver_addr,
                               uint16_t receiver_port, const uint8_t *data, uint16_t datalen) {
    if (datalen < sizeof(control_message_t)) {
        return;
    }
    /* The first byte selects the packed payload layout. Length checks protect the
     * subsequent casts; multibyte fields are converted when used on the host.
     */
    uint8_t type = data[0];

    if (type == MSG_TYPE_BEACON) {
        control_message_t *msg = (control_message_t *)data;
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
                uip_ipaddr_copy(&best_slave_candidate, sender_addr);
            }
        } else if (state == STATE_BOUND) {
            if (uip_ipaddr_cmp(sender_addr, &bound_slave_ip)) {
                current_parent_score = score;
                /* Record receipt time independently of event delivery. A missed wakeup must
                 * not make a recently heard parent appear dead.
                 */
                last_parent_contact_seconds = clock_seconds();
                if (pipeline_phase_decode(msg, &parent_cycle_start)) {
                    phase_pending = true;
                }
                process_post(&sensor_node_process, PROCESS_EVENT_SYNC_BEACON,
                             NULL); /* Remember the parent cycle reference for the next sample
                                       timer update. */
                process_post(&keepalive_process, PROCESS_EVENT_MSG, NULL);
            } else {
                if (score > current_parent_score + HYSTERESIS_MARGIN) {
                    pipeline_schedule_reset_eb();
                    LOG_INFO("Migration! Better Slave %u found (Score %d vs %d)\n",
                             uip_ntohs(msg->node_id), score, current_parent_score);
                    uip_ipaddr_copy(&best_slave_candidate, sender_addr);
                    best_score = score;
                    phase_pending = false;
                    is_bound = false;
                    /* Stop normal sampling while the parent transaction is unresolved.
                     * Binding is completed only when an acceptable OFFER arrives.
                     */
                    state = STATE_WAITING_OFFER;

                    join_multicast = false;
                    process_poll(&sensor_node_process);
                }
            }
        }
    } else if (type == MSG_TYPE_OFFER && state == STATE_WAITING_OFFER) {
        /* A directed JOIN accepts an OFFER only from its selected candidate.
         * A multicast probe has no preselected responder.
         */
        if (!join_multicast && !uip_ipaddr_cmp(sender_addr, &best_slave_candidate)) {
            LOG_WARN("JOIN ignoring OFFER from non-selected parent\n");
            return;
        }
        last_parent_contact_seconds = clock_seconds();
        control_message_t *msg = (control_message_t *)data;
        pipeline_schedule_reset_eb();
        LOG_INFO("Bound to Slave %u (Lvl %u).\n", uip_ntohs(msg->node_id), msg->level);
        uip_ipaddr_copy(&bound_slave_ip, sender_addr);
        current_parent_score = best_score;
        phase_pending = false; /* OFFER supplies no phase reference. */
        is_bound = true;
        state = STATE_BOUND;

        process_post(&sensor_node_process, PROCESS_EVENT_SYNC_BEACON,
                     NULL); /* Restart local timing after binding; a later beacon supplies
                               phase information. */
        process_poll(&keepalive_process);
        process_poll(&sensor_node_process);
    }
}

/* Check application-parent liveness once per second. TSCH may remain
 * associated even when application beacons from that parent stop arriving.
 */
PROCESS_THREAD(keepalive_process, ev, data) {
    static struct etimer ka_timer;
    /* Protothreads resume inside this function after a wait. State needed across
     * a yield must live in static storage, not an ordinary automatic local.
     */
    PROCESS_BEGIN();
    etimer_set(&ka_timer, CLOCK_SECOND);
    while (1) {
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&ka_timer));
        if (is_bound && (unsigned long)(clock_seconds() - last_parent_contact_seconds) >=
                            (unsigned long)(PARENT_TIMEOUT / CLOCK_SECOND)) {
            pipeline_schedule_reset_eb();
            LOG_WARN("Parent Timeout! Disconnecting...\n");
            phase_pending = false;
            is_bound = false;
            state = STATE_DISCOVERING;
            process_poll(&sensor_node_process);
        }
        etimer_set(&ka_timer, CLOCK_SECOND);
    }
    PROCESS_END();
}

PROCESS_THREAD(sensor_node_process, ev, data) {
    static struct etimer sync_timer;
    static struct etimer check_timer;
    static uip_ipaddr_t multicast_ip;

    static struct etimer join_timer;
    static clock_time_t join_started;
    static clock_time_t join_remaining;
    static uint8_t join_attempt;
    static clock_time_t next_delay;
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

    my_sensor_id = mac_to_node_id();
    /* Deterministic spread, 0.25..0.5 s after parent cycle start. */
    /* Spread nominal sample times within the first half-second of the parent
     * cycle. This is deterministic jitter, not collision-free slot allocation.
     */
    sample_phase_offset = CLOCK_SECOND / 4 + my_sensor_id % (CLOCK_SECOND / 4);
    LOG_INFO("Sensor started with ID: %u\n", my_sensor_id);

#if USE_CSV_TRACES == 1
    /* The supplied Cooja setup encodes its mote number in the last MAC byte.
     * This mapping selects trace data only; routing does not depend on that byte.
     */
    int id_node_cooja = linkaddr_node_addr.u8[LINKADDR_SIZE - 1];
    if (id_node_cooja >= SENSOR_ID_OFFSET) {
        mapped_csv_col = (id_node_cooja - SENSOR_ID_OFFSET) % TRACE_SENSORS;
    } else {
        mapped_csv_col = 0;
    }
#    if !PIPE_COMPACT_LOG
    LOG_INFO("CSV Mode Enabled! Mapped to Trace Column: %u\n", mapped_csv_col);
#    endif
#endif

    /* Listen on the client port for parent BEACON/OFFER traffic and send toward
     * the server port. Other multicast application ports have no listener here.
     */
    simple_udp_register(&udp_conn, UDP_CLIENT_PORT, NULL, UDP_SERVER_PORT, sensor_rx_callback);
    data_payload.node_id = uip_htons(my_sensor_id);

    etimer_set(&check_timer, CLOCK_SECOND / 2);
    while (uip_ds6_get_link_local(ADDR_PREFERRED) == NULL) {
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&check_timer));
        etimer_reset(&check_timer);
    }
    uip_create_linklocal_allnodes_mcast(&multicast_ip);

    state = STATE_DISCOVERING;
    while (1) {
        is_bound = false;

        while (state != STATE_BOUND) {
            if (state == STATE_DISCOVERING) {
                best_score = -10000;
                /* Collect advertisements for ten seconds before selecting the best candidate.
                 * Incoming callbacks may update the candidate while this process is waiting.
                 */
                etimer_set(&sync_timer, CLOCK_SECOND * 10);
                while (state == STATE_DISCOVERING && !etimer_expired(&sync_timer)) {
                    PROCESS_WAIT_EVENT();
                }

                if (best_score != -10000) {
                    join_multicast = false;
#if !PIPE_COMPACT_LOG
                    LOG_INFO("JOIN selected parent; starting transaction\n");
#endif
                } else {
                    join_multicast = true;
                    LOG_WARN("No parent found; start multicast JOIN probe\n");
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
                            MSG_TYPE_JOIN, uip_htons(my_sensor_id), 0, 0, 0, 0};
                        join_attempt++;
#if !PIPE_COMPACT_LOG
                        LOG_INFO("JOIN attempt=%u multicast=%u\n", join_attempt, join_multicast);
#endif
                        safe_udp_send(&udp_conn, &request, sizeof(request),
                                      join_multicast ? &multicast_ip : &best_slave_candidate);
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
                    LOG_WARN("JOIN timeout after %u attempts; return to discovery\n", join_attempt);
                    state = STATE_DISCOVERING;
                }
            }
        }

        etimer_set(&sync_timer, AGGREGATION_TICK);
        while (is_bound) {
            /* Beacon reception latches a correction, never triggers immediate DATA. */
            PROCESS_WAIT_UNTIL(etimer_expired(&sync_timer) || !is_bound);
            if (!is_bound) {
                break;
            }

            /* Restore full T after any shortened alignment interval, skip missed slots. */
            next_delay = AGGREGATION_TICK -
                         ((clock_time_t)(clock_time() - etimer_expiration_time(&sync_timer)) %
                          AGGREGATION_TICK);
#if PIPE_SAMPLE_PHASE_FIX
            next_delay = pipeline_sample_delay(next_delay);
#endif
            etimer_set(&sync_timer, next_delay);

            data_payload.msg_type = MSG_TYPE_DATA;
            /* Advance the application sequence for a newly generated batch. This byte
             * wraps naturally and is not a globally unique or persistent sample identifier.
             */
            current_seq_no++;
            data_payload.seq_no = current_seq_no;

#if USE_CSV_TRACES == 1
            /* One CSV scalar occupies the first DATA element. Clear the remaining
             * elements so old payload contents cannot look like additional signal samples.
             */
            uint16_t val = sensor_traces[mapped_csv_col][current_trace_row];
            data_payload.raw_data_net[0] = uip_htons(val);
            for (int i = 1; i < FFT_SAMPLES; i++) {
                data_payload.raw_data_net[i] = 0;
            }

/* Print fixed-point hundredths without linking floating-point formatting. */
#    if !PIPE_COMPACT_LOG
            LOG_INFO("Tx DATA (Seq %u) Value: %u.%02u\n", current_seq_no, val / 100, val % 100);
#    endif
            current_trace_row = (current_trace_row + 1) % TRACE_SAMPLES;
#else
            LOG_INFO("Tx DATA (Seq %u)\n", current_seq_no);
            for (int i = 0; i < FFT_SAMPLES; i++) {
                uint16_t rand_val = (random_rand() % 50) *
                                    100; /* Measurements use hundredths of the original unit. */
                data_payload.raw_data_net[i] = uip_htons(rand_val);
            }
#endif
            LOG_INFO("Tx DATA (Seq %u) UPWARDS\n", current_seq_no);

            safe_udp_send(&udp_conn, &data_payload, sizeof(data_payload_t), &bound_slave_ip);
            /* Apply the most recently decoded phase after the sample has been sent.
             * The delay guard moves tiny positive corrections to the next cycle.
             */
            if (phase_pending) {
                phase_pending = false;
                next_delay =
                    (sample_phase_offset + AGGREGATION_TICK -
                     ((clock_time_t)(clock_time() - parent_cycle_start) % AGGREGATION_TICK)) %
                    AGGREGATION_TICK;
#if PIPE_SAMPLE_PHASE_FIX
                next_delay = pipeline_sample_delay(next_delay);
#else
                if (next_delay == 0) {
                    next_delay = AGGREGATION_TICK;
                }
#endif
                etimer_set(&sync_timer, next_delay);
                LOG_INFO("PHASE sensor next_in=%lu offset=%lu\n", (unsigned long)next_delay,
                         (unsigned long)sample_phase_offset);
            }
        }
    }
    PROCESS_END();
}
