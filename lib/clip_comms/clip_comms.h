#ifndef CLIP_COMMS_H
#define CLIP_COMMS_H

#include <stdint.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up ESP-NOW on the clip (STA mode, no AP connection) and register the
// broadcast peer so the hub's MSG_BEACON frames are received while scanning.
void clip_comms_begin(void);

// Hop WiFi channels listening for the hub's beacon, up to `timeout_ms`.
// Returns true once the hub's MAC and channel are known.
bool clip_comms_find_hub(uint32_t timeout_ms);

// Housekeeping, called from loop(). Reserved for later phases.
void clip_comms_loop(void);

// Query the discovery result.
bool clip_comms_is_connected(void);
void clip_comms_hub_mac(uint8_t out[6]);
uint8_t clip_comms_channel(void);


// Sends a single HELLO to the hub. Returns true if the frame was queued.
bool clip_comms_send_hello(void);

// Send HELLO and wait for the hub's acknowledgement, retrying until
// `timeout_ms`. Returns true once the hub confirms it registered the clip.
bool clip_comms_register(uint32_t timeout_ms);

// True once the hub has acknowledged our HELLO.
bool clip_comms_is_registered(void);

// The node id the hub assigned us (valid after registration).
uint8_t clip_comms_node_id(void);

// True once the hub has sent us a MSG_ASSIGN (id + time + schedule stored).
bool clip_comms_is_assigned(void);

// The schedule from the last MSG_ASSIGN, or NULL if not assigned yet.
const week_schedule* clip_comms_schedule(void);

// Drain a pending "blink" request from the hub (identify this clip). Returns
// true and fills `seconds` if one arrived since the last call.
bool clip_comms_take_blink(uint8_t* seconds);

// Adherence events -> hub. Each queues the event for reliable delivery: it is
// retried (with backoff) until the hub ACKs it, so a "pill taken" is not lost
// during a link outage. Returns true if queued; false if the hub is unknown or
// the queue is full. The queue is kept in RTC memory, so pending events survive
// deep sleep.
bool clip_comms_send_taken(void);
bool clip_comms_send_missed(void);
bool clip_comms_send_box_open(void);

#ifdef __cplusplus
}
#endif

#endif // CLIP_COMMS_H
