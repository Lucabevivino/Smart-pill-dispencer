#ifndef COMMS_RING_H
#define COMMS_RING_H

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Single-producer / single-consumer ring buffer for inbound ESP-NOW frames.
//
// The ESP-NOW receive callback (producer, ESP-NOW task) pushes a frame; the
// main loop (consumer) pops it. This replaces the old single-slot pending
// buffers, which dropped a second same-type message arriving before the loop
// drained the first. Unlike a single slot, a burst of frames is retained.
// ---------------------------------------------------------------------------

// Largest wire message (assign_msg) so every frame fits in a slot.
#define COMMS_RING_MAX_PAYLOAD (sizeof(assign_msg))

// Slots per ring. 8 is enough for a burst of HELLOs / ACKs / events.
#ifndef COMMS_RING_SIZE
#define COMMS_RING_SIZE 8
#endif

typedef struct {
    uint8_t mac[6];                            // sender MAC (6 bytes)
    uint8_t data[COMMS_RING_MAX_PAYLOAD];      // raw frame bytes
    uint8_t len;                               // bytes actually used in `data`
} comms_ring_slot_t;

typedef struct {
    comms_ring_slot_t slots[COMMS_RING_SIZE];
    volatile uint8_t head;   // next write slot (producer: ESP-NOW callback)
    volatile uint8_t tail;   // next read slot (consumer: main loop)
} comms_ring_t;

// Push one frame (copied). Drops the frame and returns false when full.
// `len` is clamped to COMMS_RING_MAX_PAYLOAD.
bool comms_ring_push(comms_ring_t* r, const uint8_t* mac,
                     const uint8_t* data, uint8_t len);

// Pop the oldest frame. Returns false when empty. NULL out-args are skipped.
bool comms_ring_pop(comms_ring_t* r, uint8_t* mac, uint8_t* data, uint8_t* len);

#ifdef __cplusplus
}
#endif

#endif // COMMS_RING_H
