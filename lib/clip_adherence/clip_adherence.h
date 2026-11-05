#ifndef CLIP_ADHERENCE_H
#define CLIP_ADHERENCE_H

#include <stdint.h>
#include "protocol.h"   // week_schedule

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Adherence state machine on the clip. Watches the (simulated) box sensor and
// drives the alert + hub reports through two coordinated state machines:
//
//   box    : CLOSED -> WAITING_FOR_OPENING -> OPEN -> WAITING_FOR_CLOSING
//   intake : WAITING -> CONSUMED | MISSED
//
// The clip_alarm module is the pure output driver; this module is the brain that
// decides when to sound/stop it and when to send MSG_TAKEN / MSG_MISSED /
// MSG_BOX_OPEN to the hub.
// ---------------------------------------------------------------------------

// Configure the box sensor pin (only used when simulation is off).
void clip_adherence_begin(uint8_t sensor_pin);

// Set the weekly schedule to track (deep-copied). NULL clears the schedule and
// resets both state machines (e.g. the hub removed this clip).
void clip_adherence_set_schedule(const week_schedule* sched);

// Housekeeping — call from loop().
void clip_adherence_loop(void);

#ifdef __cplusplus
}
#endif

#endif // CLIP_ADHERENCE_H
