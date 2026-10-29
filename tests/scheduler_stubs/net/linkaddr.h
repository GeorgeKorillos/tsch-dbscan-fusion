#ifndef TEST_LINKADDR
#define TEST_LINKADDR
#include <stdint.h>
#include <string.h>
#define LINKADDR_SIZE 8
typedef struct {
    uint8_t u8[8];
} linkaddr_t;
extern linkaddr_t linkaddr_node_addr;
extern const linkaddr_t linkaddr_null;
#define linkaddr_cmp(a, b) (!memcmp(a, b, sizeof(linkaddr_t)))
#endif
