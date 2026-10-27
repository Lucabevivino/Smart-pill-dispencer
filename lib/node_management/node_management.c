#include "node_management.h"
#include <string.h>

// The hub's clips. Static storage is zero-initialized, so every node starts
// undiscovered and free at boot.
static Node nodes[MAX_NODES];

int node_reserve_free(void) {
    for (int i = 0; i < MAX_NODES; i++) {
        if (nodes[i].discovered && !nodes[i].assigned) {
            nodes[i].assigned = 1;
            return i + 1;
        }
    }
    return 0;
}

void node_release(int id) {
    if (id >= 1 && id <= MAX_NODES) {
        nodes[id - 1].assigned = 0;
    }
}

int node_register(const uint8_t mac[6], uint32_t now_ms) {
    // Refresh an already-known MAC.
    for (int i = 0; i < MAX_NODES; i++) {
        if (nodes[i].discovered && memcmp(nodes[i].mac, mac, 6) == 0) {
            nodes[i].last_seen = now_ms;
            return i + 1;
        }
    }
    // Claim the first undiscovered slot.
    for (int i = 0; i < MAX_NODES; i++) {
        if (!nodes[i].discovered) {
            memcpy(nodes[i].mac, mac, 6);
            nodes[i].discovered = 1;
            nodes[i].assigned = 0;
            nodes[i].last_seen = now_ms;
            return i + 1;
        }
    }
    return 0;   // registry full
}

int node_get_mac(int id, uint8_t mac_out[6]) {
    if (id < 1 || id > MAX_NODES || !nodes[id - 1].discovered) {
        return 0;
    }
    memcpy(mac_out, nodes[id - 1].mac, 6);
    return 1;
}

int node_forget_stale(uint32_t now_ms, uint32_t stale_ms) {
    int forgotten = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        if (nodes[i].discovered && !nodes[i].assigned &&
            now_ms - nodes[i].last_seen >= stale_ms) {
            memset(&nodes[i], 0, sizeof(Node));
            forgotten++;
        }
    }
    return forgotten;
}
