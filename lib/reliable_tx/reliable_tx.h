#ifndef RELIABLE_TX_H
#define RELIABLE_TX_H

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Reliable outbound ESP-NOW delivery (send-with-retry-until-ACK).
//
// A frame is queued and re-transmitted until the peer acknowledges it with an
// application-level MSG_ACK carrying the same `seq`, so data is not lost when
// the link is down. Each frame retries independently on an exponential-backoff
// schedule — no stop-and-wait, so one stuck destination never blocks another.
//
// The caller owns the storage, which lets the clip keep its queue in RTC slow
// memory (survives deep sleep) while the hub keeps it in ordinary RAM.
//
// Delivery is confirmed by two ESP-NOW signals together:
//   - the send callback (esp_now_register_send_cb): ESP_NOW_SEND_FAIL means the
//     link-layer transmit failed -> retry immediately;
//   - an application MSG_ACK within the backoff deadline -> link-layer "success"
//     alone is not proof the receiver processed the frame, so an unacked frame
//     is retried at the capped interval (never silently dropped).
// ---------------------------------------------------------------------------

// Largest reliably-delivered message (assign_msg) fits in a queue slot.
#define RELIABLE_MAX_PAYLOAD (sizeof(assign_msg))

#ifndef RELIABLE_QUEUE_SIZE
#define RELIABLE_QUEUE_SIZE 8
#endif

typedef struct {
    bool     used;           // slot holds a queued frame
    uint8_t  dst[6];         // destination MAC
    uint8_t  data[RELIABLE_MAX_PAYLOAD];
    uint8_t  len;
    uint8_t  seq;            // sender-assigned sequence, echoed by the ACK
    uint8_t  attempts;       // transmissions so far (drives the backoff)
    uint32_t next_retry_ms;  // millis() deadline for the next (re)transmit
} reliable_frame_t;

typedef struct {
    // magic distinguishes a valid RTC-memory queue from a cold boot (zeroed).
    uint32_t magic;
    reliable_frame_t frames[RELIABLE_QUEUE_SIZE];
    uint8_t  next_seq;              // monotonically increasing per-instance seq
    uint32_t base_retry_ms;         // e.g. 1000
    uint32_t max_retry_ms;          // e.g. 30000 (retry forever at this cap)
    bool (*send_fn)(const uint8_t* dst, const uint8_t* data, uint8_t len);
} reliable_tx_t;

// Initialize. Re-points `send_fn` and resets timing, but keeps any queued frames
// when the queue is a valid RTC-memory queue from before deep sleep (magic set);
// on a cold boot (magic 0) the queue starts empty.
void reliable_init(reliable_tx_t* t,
                   bool (*send_fn)(const uint8_t* dst, const uint8_t* data, uint8_t len));

// Queue `data` (`len` bytes) for `dst` and transmit it once immediately.
// Returns false when the queue is full (the newest frame is dropped).
//
// Convention: every reliable message carries its `seq` as the SECOND byte
// (`data[0]` is the message type). reliable_send overwrites `data[1]` with the
// freshly assigned seq, so callers build the rest of the frame and leave the
// seq byte unset.
bool reliable_send(reliable_tx_t* t, const uint8_t* dst, const void* data, uint8_t len);

// Housekeeping — call from the main loop. Retransmits any frame whose retry
// deadline has passed, advancing its backoff.
void reliable_loop(reliable_tx_t* t);

// Feed the ESP-NOW send callback result in. On a link-layer failure, frames to
// `mac` are marked for immediate retry.
void reliable_on_sent(reliable_tx_t* t, const uint8_t* mac, bool ok);

// Handle an application ACK carrying `seq`: free the matching frame.
void reliable_on_ack(reliable_tx_t* t, uint8_t seq);

#ifdef __cplusplus
}
#endif

#endif // RELIABLE_TX_H
