#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Shared wire protocol between the Hub (ESP32) and the Clips (ESP32-C3).
// Both are little-endian and compiled with the same gcc family, so packed
// structs are sent verbatim over ESP-NOW (max payload: 250 bytes).
// ---------------------------------------------------------------------------

// --- Shared schedule model ---------------------------------------------------
#define MAX_INTAKES_PER_DAY 4
#define DAYS_PER_WEEK       7

// One day's intake schedule: how many times the pill is taken and at which
// hour/minute each intake happens.
typedef struct __attribute__((packed)) {
    uint8_t count;                          // intakes that day (0 = none)
    uint8_t hours[MAX_INTAKES_PER_DAY];     // 0-23
    uint8_t minutes[MAX_INTAKES_PER_DAY];   // 0-59
} day_schedule;

// A full week, index 0 = Monday ... 6 = Sunday.
typedef struct __attribute__((packed)) {
    day_schedule days[DAYS_PER_WEEK];
} week_schedule;

// --- Wall-clock time ---------------------------------------------------------
// Local time, already timezone-adjusted by the hub (which is on WiFi/NTP).
// `weekday` is 0 = Monday ... 6 = Sunday, so it indexes week_schedule directly.
typedef struct __attribute__((packed)) {
    uint16_t year;    // e.g. 2026
    uint8_t  mon;     // 1-12
    uint8_t  mday;    // 1-31
    uint8_t  hour;    // 0-23
    uint8_t  min;     // 0-59
    uint8_t  sec;     // 0-59
    uint8_t  weekday; // 0 = Monday ... 6 = Sunday
} hub_time;

// --- Message types -----------------------------------------------------------
typedef enum {
    MSG_BEACON        = 1,  // hub -> broadcast : hub present, unassigned nodes reply
    MSG_HELLO         = 2,  // node -> hub      : "I want to connect"
    MSG_BLINK         = 3,  // hub -> node      : blink the LED (identify a clip)
    MSG_ASSIGN        = 4,  // hub -> node      : node id + schedule + time
    MSG_TIME_REQUEST  = 5,  // node -> hub      : "send me the current time"
    MSG_TIME_RESPONSE = 6,  // hub -> node      : current time
    MSG_REMOVE        = 7,  // hub -> node      : clear id/schedule/alarms
} hub_msg_type;

#define PROTOCOL_VERSION 1

// Every message starts with a `type` byte so the receiver can dispatch on it.
typedef struct __attribute__((packed)) { uint8_t type; uint8_t version; } beacon_msg;
typedef struct __attribute__((packed)) { uint8_t type; uint8_t version; } hello_msg;
typedef struct __attribute__((packed)) { uint8_t type; uint8_t seconds; } blink_msg;
typedef struct __attribute__((packed)) {
    uint8_t      type;
    uint8_t      node_id;
    hub_time     time;
    week_schedule schedule;
} assign_msg;
typedef struct __attribute__((packed)) { uint8_t type; } time_request_msg;
typedef struct __attribute__((packed)) { uint8_t type; hub_time time; } time_response_msg;
typedef struct __attribute__((packed)) { uint8_t type; uint8_t node_id; } remove_msg;

// ESP-NOW broadcast address (FF:FF:FF:FF:FF:FF).
extern const uint8_t ESPNOW_BROADCAST_MAC[6];

#ifdef __cplusplus
}
#endif

#endif // PROTOCOL_H
