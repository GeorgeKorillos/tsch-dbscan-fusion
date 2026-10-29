#ifndef TEST_TSCH
#define TEST_TSCH
#include "contiki.h"
#include "net/linkaddr.h"
extern int tsch_is_associated;
extern const linkaddr_t tsch_broadcast_address;
#define LINK_OPTION_TX 1
#define LINK_OPTION_RX 2
#define LINK_OPTION_SHARED 4
enum link_type { LINK_TYPE_NORMAL, LINK_TYPE_ADVERTISING };
struct tsch_neighbor {
    int unused;
};
struct tsch_slotframe {
    uint16_t handle, size;
};
struct tsch_link {
    struct tsch_slotframe *sf;
    uint16_t slot;
    uint8_t options, active;
};
int tsch_schedule_remove_all_slotframes(void);
struct tsch_slotframe *tsch_schedule_add_slotframe(uint16_t, uint16_t);
struct tsch_link *tsch_schedule_add_link(struct tsch_slotframe *, uint8_t, enum link_type,
                                         const linkaddr_t *, uint16_t, uint16_t, int);
struct tsch_link *tsch_schedule_get_link_by_timeslot(struct tsch_slotframe *, uint16_t);
int tsch_schedule_remove_link(struct tsch_slotframe *, struct tsch_link *);
int tsch_queue_global_packet_count(void);
void tsch_set_eb_period(uint32_t);
#endif
