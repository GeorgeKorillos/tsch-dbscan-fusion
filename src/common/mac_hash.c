#include "mac_hash.h"

#include "net/linkaddr.h"

/* Hash the full link-layer address with FNV-1a, then fold it into 16 bits. */
uint16_t mac_to_node_id(void) {
    uint32_t hash = 2166136261u; /* FNV offset basis */

    for (int i = 0; i < LINKADDR_SIZE; i++) {
        hash ^= linkaddr_node_addr.u8[i];
        hash *= 16777619; /* FNV prime */
    }

    /* Fold the high and low halves into the application ID. */
    return (uint16_t)(hash ^ (hash >> 16));
}
