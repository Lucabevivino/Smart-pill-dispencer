#include "node_management.h"
#include <string.h>
#include "nvs.h"
#include "esp_err.h"

// The hub's clips. Static storage is zero-initialized, so every node starts
// undiscovered and free at boot.
static Node nodes[MAX_NODES];

// NVS namespace/key holding the registry blob. `last_seen` is runtime state, so
// node_restore() resets it (see below).
#define NODE_NVS_NAMESPACE "nodes"
#define NODE_NVS_KEY       "registry"

// Write the registry to NVS so it survives a reboot. Called only on real changes
// (new node, assign, release, forget), never on a mere last_seen refresh.
static void node_persist(void) {
    nvs_handle_t h;
    if (nvs_open(NODE_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, NODE_NVS_KEY, nodes, sizeof(nodes));
    nvs_commit(h);
    nvs_close(h);
}

void node_restore(void) {
    nvs_handle_t h;
    if (nvs_open(NODE_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;   // nothing persisted (or flash unavailable)
    }

    size_t len = sizeof(nodes);
    if (nvs_get_blob(h, NODE_NVS_KEY, nodes, &len) == ESP_OK && len == sizeof(nodes)) {
        for (int i = 0; i < MAX_NODES; i++) {
            nodes[i].last_seen = 0;   // re-announce before being pruned as stale
        }
    } else {
        memset(nodes, 0, sizeof(nodes));   // size mismatch: ignore stale data
    }
    nvs_close(h);
}

int node_reserve_free(void) {
    for (int i = 0; i < MAX_NODES; i++) {
        if (nodes[i].discovered && !nodes[i].assigned) {
            nodes[i].assigned = 1;
            node_persist();
            return i + 1;
        }
    }
    return 0;
}

void node_release(int id) {
    if (id >= 1 && id <= MAX_NODES) {
        nodes[id - 1].assigned = 0;
        node_persist();
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
            node_persist();
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
    if (forgotten) {
        node_persist();
    }
    return forgotten;
}
