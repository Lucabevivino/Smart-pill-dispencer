#ifndef NODE_MANAGEMENT_H
#define NODE_MANAGEMENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Number of clips (nodes) the hub can track. A slot's 1-based index is the
// node id that gets sent to a clip.
#define MAX_NODES 4

typedef struct Node {
    uint8_t  mac[6];      // ESP-NOW peer address
    uint8_t  discovered;  // hub has heard a HELLO from this node
    uint8_t  assigned;    // associated with a pill
    uint32_t last_seen;   // millis() when the node last announced itself
} Node;

// Reserve the first discovered-but-unassigned node, mark it assigned and return
// its 1-based id, or 0 if none is free.
int node_reserve_free(void);

// Mark node `id` (1-based) as unassigned (still discovered, so it can be
// re-assigned later).
void node_release(int id);

// Register a node that sent a HELLO: refresh its slot if the MAC is already
// known, otherwise claim the first undiscovered slot. Returns the 1-based id,
// or 0 if the registry is full.
int node_register(const uint8_t mac[6], uint32_t now_ms);

// Copy node `id`'s MAC into `mac_out`. Returns 1 if the node is discovered.
int node_get_mac(int id, uint8_t mac_out[6]);

// Forget unassigned nodes not heard from for `stale_ms`. Returns the number of
// nodes forgotten. Assigned nodes are left alone (a pill still references them).
int node_forget_stale(uint32_t now_ms, uint32_t stale_ms);

#ifdef __cplusplus
}
#endif

#endif // NODE_MANAGEMENT_H
