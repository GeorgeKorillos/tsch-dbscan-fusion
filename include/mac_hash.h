#ifndef MAC_HASH_H_
#define MAC_HASH_H_

#include <stdint.h>

/* Derive a compact application ID from the link-layer address. Hash collisions remain
 * possible. */
uint16_t mac_to_node_id(void);

#endif