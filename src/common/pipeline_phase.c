#include "pipeline_phase.h"

#include "net/ipv6/uip.h"
#include "net/mac/tsch/tsch.h"

/* The offset alone excludes only application jitter, not time in MAC queues.
 * Carry the network-time sample taken at enqueue and add its age on receipt.
 * This is soft alignment (clock tick resolution / timesync error), not a
 * packet TX timestamp nor a shared sample epoch. No 5s/15ms integer assumption.
 */
void pipeline_phase_stamp(control_message_t *msg, clock_time_t cycle_start) {
    uint32_t sent = (uint32_t)tsch_get_network_uptime_ticks();
    msg->phase_sent_net = uip_htonl(sent);
    msg->phase_offset_net =
        uip_htons((uint16_t)((clock_time_t)(clock_time() - cycle_start) % AGGREGATION_TICK));
}

int pipeline_phase_decode(const control_message_t *msg, clock_time_t *cycle_start) {
    uint32_t age;
    uint16_t offset = uip_ntohs(msg->phase_offset_net);
    if (!tsch_is_associated || offset >= AGGREGATION_TICK) {
        return 0;
    }
    age = (uint32_t)tsch_get_network_uptime_ticks() - uip_ntohl(msg->phase_sent_net);
    /* Reject stale packets or incompatible time references. Unsigned subtraction
     * handles the low-32-bit network tick wrap for these short packet ages. */
    if (age >= AGGREGATION_TICK) {
        return 0;
    }
    *cycle_start = (clock_time_t)(clock_time() - (clock_time_t)((offset + age) % AGGREGATION_TICK));
    return 1;
}
