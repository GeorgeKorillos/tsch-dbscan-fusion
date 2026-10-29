#include "net/linkaddr.h"
enum { PACKETBUF_ATTR_TSCH_SLOTFRAME, PACKETBUF_ATTR_TSCH_TIMESLOT, PACKETBUF_ATTR_FRAME_TYPE };
#define PACKETBUF_ADDR_RECEIVER 0
void packetbuf_set_attr(int, uint16_t);
uint16_t packetbuf_attr(int);
const linkaddr_t *packetbuf_addr(int);
