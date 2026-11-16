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
// Queued for reliable delivery (retried until the clip ACKs). Returns true if
// the message was queued.
bool hub_comms_assign(int node_id, const week_schedule* schedule);

// Make node `node_id` blink for `seconds` (used while adding a pill). Reliable.
bool hub_comms_blink(int node_id, uint8_t seconds);

// Tell node `node_id` it has been removed. Reliable.
bool hub_comms_remove(int node_id);

// Register a callback that returns the schedule for an assigned node (1-based),
// or NULL if the node has no schedule. Used to re-ASSIGN a rebooted clip that
// re-HELLOs after losing its schedule (a cold power cycle wipes the clip's RTC
// schedule, so the hub must re-send it — otherwise the clip keeps its node_id
// but never alarms again).
void hub_comms_set_schedule_source(const week_schedule* (*cb)(int node_id));

// Current synced local time. Returns false if the clock is not synced yet.
// (Unused by the UI currently; kept internal — see fetch_time in hub_comms.cpp.)

// Drain the next adherence/battery event the hub received from a clip
// (MSG_TAKEN, MSG_MISSED, MSG_BOX_OPEN or MSG_BATTERY). Returns true and fills
// `type` + `node_id` (+ `mv` for MSG_BATTERY) if one is pending. Events are
// queued (not single-slot), so simultaneous clips don't clobber each other.
bool hub_comms_take_event(uint8_t* type, uint8_t* node_id, uint16_t* mv);

// Battery voltage (millivolts) last reported by `node_id` (1-based), or 0 if the
// clip has not reported one yet (or has battery sensing disabled).
uint16_t hub_comms_battery_mv(int node_id);

// --- adherence log (server-ready) --------------------------------------------
// Every fresh adherence event (MSG_TAKEN / MSG_MISSED / MSG_BOX_OPEN) is appended
// to a persistent, bounded ring log that survives reboot. Tomorrow's server
// client drains this with count()/get() and clears it after forwarding.

// Number of events currently in the log.
uint16_t hub_comms_adherence_count(void);

// Copy log entry `index` (0..count-1, oldest first) into the out-params (any may
// be NULL). Returns true if the index is in range.
bool hub_comms_adherence_get(uint16_t index, uint8_t* node_id, uint8_t* type,
                             uint8_t* seq, uint32_t* timestamp);

// Drop all logged events. The per-node de-dup watermark is kept, so clearing the
// server-facing log does not reopen the double-count window.
void hub_comms_adherence_clear(void);

#ifdef __cplusplus
}
#endif

#endif // HUB_COMMS_H
