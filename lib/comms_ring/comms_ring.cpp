#include "comms_ring.h"
#include <string.h>

bool comms_ring_push(comms_ring_t* r, const uint8_t* mac,
                     const uint8_t* data, uint8_t len) {
    if (len > COMMS_RING_MAX_PAYLOAD) {
        len = COMMS_RING_MAX_PAYLOAD;   // guard against oversized frames
    }
    uint8_t next = (uint8_t)((r->head + 1) % COMMS_RING_SIZE);
    if (next == r->tail) {
        return false;   // full — drop the newest rather than clobber a pending one
    }

    comms_ring_slot_t* s = &r->slots[r->head];
    memcpy(s->mac, mac, 6);
    memcpy(s->data, data, len);
    s->len = len;
    // Publish the slot by advancing head only after the payload is fully written.
    r->head = next;
    return true;
}

bool comms_ring_pop(comms_ring_t* r, uint8_t* mac, uint8_t* data, uint8_t* len) {
    if (r->head == r->tail) {
        return false;   // empty
    }
    comms_ring_slot_t* s = &r->slots[r->tail];
    if (mac != NULL) {
        memcpy(mac, s->mac, 6);
    }
    if (data != NULL) {
        memcpy(data, s->data, s->len);
    }
    if (len != NULL) {
        *len = s->len;
    }
    r->tail = (uint8_t)((r->tail + 1) % COMMS_RING_SIZE);
    return true;
}
