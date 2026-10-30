#ifndef CLIP_COMMS_H
#define CLIP_COMMS_H

#include <stdint.h>

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

#ifdef __cplusplus
}
#endif

#endif // CLIP_COMMS_H
