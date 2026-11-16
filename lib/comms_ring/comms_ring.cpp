#include "comms_ring.h"
#include <string.h>

// SPSC ring with acquire/release ordering.
//
// Producer (ESP-NOW callback) and consumer (main loop) run on different cores on
// the dual-core Hub, so the index updates must be ordered, not just `volatile`:
//   - push release-stores `head` AFTER the payload is written, so a consumer that
//     sees the new head is guaranteed to see the slot contents too;
//   - pop acquire-loads `head` BEFORE reading the slot, pairing with that release.
// `tail` is the reverse direction (consumer advances it, producer reads it).

bool comms_ring_push(comms_ring_t* r, const uint8_t* mac,
                     const uint8_t* data, uint8_t len) {
    if (len > COMMS_RING_MAX_PAYLOAD) {
        len = COMMS_RING_MAX_PAYLOAD;   // guard against oversized frames
    }
    uint8_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    uint8_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    uint8_t next = (uint8_t)((head + 1) % COMMS_RING_SIZE);
    if (next == tail) {
        return false;   // full — drop the newest rather than clobber a pending one
    }

    comms_ring_slot_t* s = &r->slots[head];
    memcpy(s->mac, mac, 6);
    memcpy(s->data, data, len);
    s->len = len;
    // Publish the slot by advancing head only after the payload is fully written.
    __atomic_store_n(&r->head, next, __ATOMIC_RELEASE);
    return true;
}

bool comms_ring_pop(comms_ring_t* r, uint8_t* mac, uint8_t* data, uint8_t* len) {
    uint8_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    uint8_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    if (head == tail) {
        return false;   // empty
    }
    comms_ring_slot_t* s = &r->slots[tail];
    if (mac != NULL) {
        memcpy(mac, s->mac, 6);
    }
    if (data != NULL) {
        memcpy(data, s->data, s->len);
    }
    if (len != NULL) {
        *len = s->len;
    }
    // Free the slot: release-store tail so the producer observes it after reading
    // the slot, and it doesn't see this index as still occupied.
    __atomic_store_n(&r->tail, (uint8_t)((tail + 1) % COMMS_RING_SIZE), __ATOMIC_RELEASE);
    return true;
}
