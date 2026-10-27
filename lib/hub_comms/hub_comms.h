#ifndef HUB_COMMS_H
#define HUB_COMMS_H

#include <stdint.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize WiFi (STA), NTP and ESP-NOW. Blocks up to `wifi_timeout_ms` for the
// WiFi link and `ntp_timeout_ms` for the first time sync, but brings ESP-NOW up
// even if those time out. Returns true on success.
bool hub_comms_begin(const char* ssid, const char* password,
                     int gmt_offset_sec, int daylight_offset_sec,
                     uint32_t wifi_timeout_ms, uint32_t ntp_timeout_ms);

// Housekeeping, called frequently: drains HELLO/time requests, reconnects WiFi,
// sends beacons, registers peers and prunes stale nodes. Safe to call from the
// main loop (it rate-limits itself).
void hub_comms_loop(void);

// Assign node `node_id` (1-based) the given schedule plus the current time.
// Returns true if the message was queued.
bool hub_comms_assign(int node_id, const week_schedule* schedule);

// Make node `node_id` blink for `seconds` (used while adding a pill).
bool hub_comms_blink(int node_id, uint8_t seconds);

// Tell node `node_id` it has been removed.
bool hub_comms_remove(int node_id);

// Current synced local time. Returns false if the clock is not synced yet.
bool hub_comms_now(hub_time* out);

#ifdef __cplusplus
}
#endif

#endif // HUB_COMMS_H
