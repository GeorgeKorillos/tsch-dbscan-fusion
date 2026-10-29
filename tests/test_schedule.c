#include "net/mac/tsch/tsch.h"
#include "net/packetbuf.h"
#include "pipeline_schedule.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
linkaddr_t linkaddr_node_addr = {{0xaa, 1, 2, 3, 4, 5, 6, 250}};
const linkaddr_t linkaddr_null = {{0}};
const linkaddr_t tsch_broadcast_address = {{255, 255, 255, 255, 255, 255, 255, 255}};
int tsch_is_associated;
static unsigned long seconds;
static int queued, allocation_fail, eb_calls;
static uint32_t eb_period;
static struct tsch_slotframe frames[2];
static struct tsch_link links[12];
static linkaddr_t dest;
static uint16_t attrs[3];
unsigned long clock_seconds(void) {
    return seconds;
}
void packetbuf_set_attr(int a, uint16_t b) {
    attrs[a] = b;
}
uint16_t packetbuf_attr(int a) {
    return attrs[a];
}
const linkaddr_t *packetbuf_addr(int a) {
    (void)a;
    return &dest;
}
void tsch_set_eb_period(uint32_t p) {
    eb_calls++;
    eb_period = p;
    assert(p <= 16 * CLOCK_SECOND);
}
int tsch_queue_global_packet_count(void) {
    return queued;
}
int tsch_schedule_remove_all_slotframes(void) {
    memset(links, 0, sizeof(links));
    return 1;
}
struct tsch_slotframe *tsch_schedule_add_slotframe(uint16_t h, uint16_t s) {
    assert(h < 2);
    frames[h].handle = h;
    frames[h].size = s;
    return &frames[h];
}
struct tsch_link *tsch_schedule_get_link_by_timeslot(struct tsch_slotframe *s, uint16_t t) {
    for (int i = 0; i < 12; i++) {
        if (links[i].active && links[i].sf == s && links[i].slot == t) {
            return &links[i];
        }
    }
    return NULL;
}
struct tsch_link *tsch_schedule_add_link(struct tsch_slotframe *s, uint8_t o, enum link_type ty,
                                         const linkaddr_t *a, uint16_t t, uint16_t c, int rm) {
    assert(s && t < s->size && c == 0 && rm == 1);
    assert(linkaddr_cmp(a, &tsch_broadcast_address));
    assert(o & LINK_OPTION_SHARED);
    if (s->handle == 0) {
        assert(ty == LINK_TYPE_ADVERTISING);
        assert(t == 0 || t == PIPE_SCHEDULE_LENGTH / 2);
    } else {
        assert(ty == LINK_TYPE_NORMAL);
        assert(t != 0 && t != PIPE_SCHEDULE_LENGTH / 2);
    }
    if (allocation_fail) {
        return NULL;
    }
    assert(tsch_schedule_get_link_by_timeslot(s, t) == NULL);
    for (int i = 0; i < 12; i++) {
        if (!links[i].active) {
            links[i] = (struct tsch_link){s, t, o, 1};
            return &links[i];
        }
    }
    return NULL;
}
int tsch_schedule_remove_link(struct tsch_slotframe *s, struct tsch_link *l) {
    assert(l->sf == s);
    l->active = 0;
    return 1;
}
static unsigned count(void) {
    unsigned n = 0;
    for (int i = 0; i < 12; i++) {
        n += links[i].active;
    }
    return n;
}
int main(void) {
#if PIPE_EB_ENABLED
    assert(TSCH_CONF_EB_PERIOD == 4 * CLOCK_SECOND);
#else
    assert(TSCH_CONF_EB_PERIOD == 0 && TSCH_CONF_MAX_EB_PERIOD == 0);
#    ifdef TSCH_CALLBACK_JOINING_NETWORK
#        error "Leaf must not install EB-ramp callbacks"
#    endif
#endif

    assert(pipeline_schedule_init());
#if PIPE_DYNAMIC_SCHEDULE
    assert(count() == 3);
    attrs[2] = 0;
    dest = linkaddr_node_addr;
    pipeline_schedule_packet_ready();
    assert(attrs[0] == 0 && attrs[1] == 65535);
    attrs[2] = 1;
    dest = linkaddr_null;
    pipeline_schedule_packet_ready();
    assert(attrs[0] == 0);
    dest = tsch_broadcast_address;
    pipeline_schedule_packet_ready();
    assert(attrs[0] == 0);
    unsigned own = 0;
    for (int i = 0; i < 12; i++) {
        if (links[i].active && links[i].sf->handle == 1) {
            own = links[i].slot;
        }
    }
    dest = linkaddr_node_addr;
    pipeline_schedule_packet_ready();
    assert(attrs[0] == 1 && attrs[1] == own && count() == 3);
    int found = 0;
    for (unsigned i = 1; i < 255; i++) {
        dest.u8[0] = (uint8_t)i;
        pipeline_schedule_packet_ready();
        if (attrs[0] == 1 && attrs[1] != own) {
            found = 1;
            break;
        }
    }
    assert(found);
    unsigned slot = attrs[1];
    assert(count() == 4);
    pipeline_schedule_packet_ready();
    assert(count() == 4 && attrs[1] == slot);
    allocation_fail = 1;
    found = 0;
    for (unsigned i = 1; i < 255; i++) {
        dest.u8[1] = (uint8_t)i;
        pipeline_schedule_packet_ready();
        if (attrs[0] == 0) {
            found = 1;
            break;
        }
    }
    assert(found && attrs[1] == 65535);
    allocation_fail = 0;
    queued = 1;
    seconds = 121;
    pipeline_schedule_maintenance();
    assert(count() == 4);
    queued = 0;
    pipeline_schedule_maintenance();
    assert(count() == 3);
    assert(tsch_schedule_get_link_by_timeslot(&frames[1], own)->options & LINK_OPTION_RX);
    /* Fill the bounded pool with new destinations: fallback must preserve live
     * cells and all selected RX hashes avoid common offsets. */
    for (unsigned i = 0; i < 4096; i++) {
        dest.u8[0] = i >> 8;
        dest.u8[1] = i;
        pipeline_schedule_packet_ready();
        if (attrs[0] == 1) {
            assert(attrs[1] > 0 && attrs[1] < PIPE_SCHEDULE_LENGTH &&
                   attrs[1] != PIPE_SCHEDULE_LENGTH / 2);
        } else {
            assert(attrs[0] == 0 && attrs[1] == 65535);
        }
        assert(count() <= 12);
    }
#else
    assert(count() == 0);
#endif
#if PIPE_ADAPTIVE_EB && PIPE_EB_ENABLED
    tsch_is_associated = 1;
    seconds = 200;
    pipeline_schedule_maintenance();
    assert(eb_period == 4 * CLOCK_SECOND);
    seconds = 259;
    pipeline_schedule_maintenance();
    assert(eb_period == 4 * CLOCK_SECOND);
    seconds = 260;
    pipeline_schedule_maintenance();
    assert(eb_period == 8 * CLOCK_SECOND);
    seconds = 320;
    pipeline_schedule_maintenance();
    assert(eb_period == 16 * CLOCK_SECOND);
    seconds = 380;
    pipeline_schedule_maintenance();
    assert(eb_period == 16 * CLOCK_SECOND);
    pipeline_schedule_reset_eb();
    pipeline_schedule_maintenance();
    assert(eb_period == 4 * CLOCK_SECOND);
    seconds = 440;
    pipeline_schedule_maintenance();
    assert(eb_period == 8 * CLOCK_SECOND);
    seconds = ULONG_MAX - 30;
    pipeline_schedule_reset_eb();
    pipeline_schedule_maintenance();
    seconds += 60;
    pipeline_schedule_maintenance();
    assert(eb_period == 8 * CLOCK_SECOND);
    tsch_is_associated = 0;
    pipeline_schedule_maintenance();
    tsch_is_associated = 1;
    pipeline_schedule_maintenance();
    assert(eb_period == 4 * CLOCK_SECOND);
#else
    pipeline_schedule_reset_eb();
    pipeline_schedule_maintenance();
    assert(eb_calls == 0);
#endif
    puts("PASS packet classification, dynamic creation/collision reuse, pool fallback, "
         "safe GC, EB ramp/reset");
    return 0;
}
