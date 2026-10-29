#include "pipeline_schedule.h"

#include "contiki.h"
#include "net/linkaddr.h"
#include "net/mac/framer/frame802154.h"
#include "net/mac/tsch/tsch.h"
#include "net/packetbuf.h"
#include "sys/log.h"

#include <string.h>

#define LOG_MODULE "PipeSched"
#define LOG_LEVEL LOG_LEVEL_INFO
#define SF_CONTROL 0
#define SF_UNICAST 1
#define ANY_SLOT 0xffff
#define SECOND_CONTROL (PIPE_SCHEDULE_LENGTH / 2)
#define LEASE_SECONDS 120UL
#define EB_DWELL_SECONDS 60UL

static uint8_t ready;
#if PIPE_ADAPTIVE_EB && PIPE_EB_ENABLED
static volatile uint8_t eb_reset;
static uint8_t was_associated;
static uint8_t eb_seconds = 4;
static unsigned long eb_since;
#endif
#if PIPE_DYNAMIC_SCHEDULE
static struct tsch_slotframe *unicast_sf;
static uint16_t own_slot;
static uint8_t leased[PIPE_SCHEDULE_LENGTH];
static unsigned long used[PIPE_SCHEDULE_LENGTH];
static unsigned long fallback_count;

/* Deterministic full-MAC hash, same on every role. Reserved control offsets
 * never overlap the unicast cells. Hash collisions remain contention, not
 * exclusive grants: unicast TX cells MUST carry the SHARED flag. */
static uint16_t receiver_slot(const linkaddr_t *addr) {
    uint16_t h = 0x811c;
    uint8_t i;
    for (i = 0; i < LINKADDR_SIZE; i++) {
        h = (uint16_t)((h ^ addr->u8[i]) * 257u);
    }
    h = (uint16_t)(h % (PIPE_SCHEDULE_LENGTH - 2)) + 1;
    if (h >= SECOND_CONTROL) {
        h++;
    }
    return h;
}
#endif

void pipeline_schedule_reset_eb(void) {
#if PIPE_ADAPTIVE_EB && PIPE_EB_ENABLED
    eb_reset = 1; /* Callbacks only latch; no etimer operations in UDP context. */
#endif
}
void pipeline_schedule_new_time_source(const struct tsch_neighbor *old,
                                       const struct tsch_neighbor *new) {
    (void)old;
    (void)new;
    pipeline_schedule_reset_eb();
}

int pipeline_schedule_packet_ready(void) {
#if PIPE_DYNAMIC_SCHEDULE
    const linkaddr_t *to;
    uint16_t slot;
    /* This hook runs before tsch_queue_add_packet takes its lock and before
     * queuebuf_new_from_packetbuf. Classification becomes per-packet metadata. */
    packetbuf_set_attr(PACKETBUF_ATTR_TSCH_SLOTFRAME, SF_CONTROL);
    packetbuf_set_attr(PACKETBUF_ATTR_TSCH_TIMESLOT, ANY_SLOT);
    if (!ready) {
        return 0;
    }
    if (packetbuf_attr(PACKETBUF_ATTR_FRAME_TYPE) == FRAME802154_BEACONFRAME) {
        return 0;
    }
    to = packetbuf_addr(PACKETBUF_ADDR_RECEIVER);
    if (linkaddr_cmp(to, &linkaddr_null) || linkaddr_cmp(to, &tsch_broadcast_address)) {
        return 0;
    }
    slot = receiver_slot(to);
    if (tsch_schedule_get_link_by_timeslot(unicast_sf, slot) == NULL) {
        if (tsch_schedule_add_link(unicast_sf, LINK_OPTION_TX | LINK_OPTION_SHARED,
                                   LINK_TYPE_NORMAL, &tsch_broadcast_address, slot, 0, 1) == NULL) {
            /* Pool/lock failure: queue on common cells, where every node listens.
             * Never reject the packet just because a dynamic cell cannot be made. */
            fallback_count++;
            pipeline_schedule_reset_eb();
            return 0;
        }
        leased[slot] = 1;
        pipeline_schedule_reset_eb();
        LOG_INFO("DYN TX cell slot=%u\n", slot);
    }
    used[slot] = clock_seconds();
    packetbuf_set_attr(PACKETBUF_ATTR_TSCH_SLOTFRAME, SF_UNICAST);
    packetbuf_set_attr(PACKETBUF_ATTR_TSCH_TIMESLOT, slot);
#endif
    return 0;
}

/* Run timer and lease work in process context, never in a UDP callback. */
void pipeline_schedule_maintenance(void) {
    if (!ready) {
        return;
    }
#if PIPE_ADAPTIVE_EB && PIPE_EB_ENABLED
    if (!tsch_is_associated) {
        was_associated = 0;
        eb_reset = 1;
    } else {
        if (!was_associated || eb_reset) {
            was_associated = 1;
            eb_reset = 0;
            eb_seconds = 4;
            eb_since = clock_seconds();
            tsch_set_eb_period((uint32_t)eb_seconds * CLOCK_SECOND);
            LOG_INFO("DYN EB period=4s reset\n");
        } else if ((unsigned long)(clock_seconds() - eb_since) >= EB_DWELL_SECONDS) {
            eb_since = clock_seconds();
            if (eb_seconds < 16) {
                eb_seconds *= 2;
                tsch_set_eb_period((uint32_t)eb_seconds * CLOCK_SECOND);
                LOG_INFO("DYN EB period=%us\n", eb_seconds);
            }
        }
    }
#endif
#if PIPE_DYNAMIC_SCHEDULE
    /* Do not remove cells while any packet can still reference them.
     * If continuously busy, defer GC and use common-cell fallback on pool full.
     * This conservative global-queue check trades memory reuse for safety. */
    if (tsch_queue_global_packet_count() == 0) {
        uint16_t s;
        for (s = 1; s < PIPE_SCHEDULE_LENGTH; s++) {
            if (s != own_slot && leased[s] &&
                (unsigned long)(clock_seconds() - used[s]) >= LEASE_SECONDS) {
                struct tsch_link *l = tsch_schedule_get_link_by_timeslot(unicast_sf, s);
                if (l == NULL || tsch_schedule_remove_link(unicast_sf, l)) {
                    leased[s] = 0;
                }
            }
        }
    }
    /* Sparse diagnostic; a full pool does not imply dropped application data. */
    if (fallback_count) {
        LOG_WARN("DYN common fallback count=%lu\n", fallback_count);
        fallback_count = 0;
    }
#endif
}

#ifndef PIPE_HOST_TEST
PROCESS(pipeline_schedule_process, "Dynamic schedule maintenance");
PROCESS_THREAD(pipeline_schedule_process, ev, data) {
    static struct etimer timer;
    PROCESS_BEGIN();
    while (1) {
        etimer_set(&timer, CLOCK_SECOND);
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timer));
        pipeline_schedule_maintenance();
    }
    PROCESS_END();
}
#endif

/* Install the same scheduling rule on every role. Addresses determine cells;
 * the scheduler does not need a preconfigured topology or node-role table. */
int pipeline_schedule_init(void) {
#if PIPE_DYNAMIC_SCHEDULE
    struct tsch_slotframe *control;

    tsch_schedule_remove_all_slotframes();
    control = tsch_schedule_add_slotframe(SF_CONTROL, PIPE_SCHEDULE_LENGTH);
    unicast_sf = tsch_schedule_add_slotframe(SF_UNICAST, PIPE_SCHEDULE_LENGTH);
    if (control == NULL || unicast_sf == NULL) {
        goto failed;
    }
    if (tsch_schedule_add_link(control, LINK_OPTION_TX | LINK_OPTION_RX | LINK_OPTION_SHARED,
                               LINK_TYPE_ADVERTISING, &tsch_broadcast_address, 0, 0, 1) == NULL) {
        goto failed;
    }
    if (tsch_schedule_add_link(control, LINK_OPTION_TX | LINK_OPTION_RX | LINK_OPTION_SHARED,
                               LINK_TYPE_ADVERTISING, &tsch_broadcast_address, SECOND_CONTROL, 0,
                               1) == NULL) {
        goto failed;
    }
    own_slot = receiver_slot(&linkaddr_node_addr);
    /* TX is included so another receiver hashing to our own slot is usable.
     * Packet selector prevents EB/broadcast leaking into this NORMAL cell. */
    if (tsch_schedule_add_link(unicast_sf, LINK_OPTION_TX | LINK_OPTION_RX | LINK_OPTION_SHARED,
                               LINK_TYPE_NORMAL, &tsch_broadcast_address, own_slot, 0, 1) == NULL) {
        goto failed;
    }
    memset(leased, 0, sizeof(leased));
    LOG_INFO("PIPE schedule dynamic=1 length=%u own_rx=%u control=0,%u adaptive_eb=%u\n",
             PIPE_SCHEDULE_LENGTH, own_slot, SECOND_CONTROL, PIPE_ADAPTIVE_EB);
#else
    LOG_INFO("PIPE schedule dynamic=0 minimal=%u adaptive_eb=%u\n", PIPE_SHARED_LENGTH,
             PIPE_ADAPTIVE_EB);
#endif
    LOG_INFO("PIPE timing=%u diagnostics=%u\n", PIPE_EXTENDED_TIMING, PIPE_DIAGNOSTICS);
    LOG_INFO("PIPE project_only=1 eb_enabled=%u leaf_no_eb=%u\n", PIPE_EB_ENABLED, PIPE_LEAF_NO_EB);
#if PIPE_SAMPLE_PHASE_FIX || PIPE_COMPACT_LOG
    LOG_INFO("PIPE sample_fix=%u compact=%u\n", PIPE_SAMPLE_PHASE_FIX, PIPE_COMPACT_LOG);
#endif
    ready = 1;
    pipeline_schedule_reset_eb();
#ifndef PIPE_HOST_TEST
#    if PIPE_DYNAMIC_SCHEDULE || (PIPE_ADAPTIVE_EB && PIPE_EB_ENABLED)
    process_start(&pipeline_schedule_process, NULL);
#    endif
#endif
    return 1;
#if PIPE_DYNAMIC_SCHEDULE
failed:
    tsch_schedule_remove_all_slotframes();
    LOG_ERR("DYN schedule init failed; MAC not started\n");
    return 0;
#endif
}
