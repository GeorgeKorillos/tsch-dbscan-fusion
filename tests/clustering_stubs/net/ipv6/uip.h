#ifndef TEST_UIP_H
#define TEST_UIP_H
#include <stdint.h>
static inline uint16_t uip_htons(uint16_t x) {
    return (uint16_t)((x << 8) | (x >> 8));
}
#define uip_ntohs uip_htons
#endif
