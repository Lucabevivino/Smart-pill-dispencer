#ifndef CLIP_ADHERENCE_H
#define CLIP_ADHERENCE_H

#include <stdint.h>
#include <time.h>
#include "protocol.h"   // week_schedule

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Adherence state machine on the clip. Watches the box sensor (simulated,
// phototransistor, or reflective IR — see CLIP_SENSOR_TYPE in clip_adherence.cpp)
// and drives the alert + hub reports through two coordinated state machines:
//
//   box    : CLOSED -> WAITING_FOR_OPENING -> OPEN -> WAITING_FOR_CLOSING
//   intake : WAITING -> CONSUMED | MISSED
//
// The clip_alarm module is the pure output driver; this module is the brain that
// decides when to sound/stop it and when to send MSG_TAKEN / MSG_MISSED /
// MSG_BOX_OPEN to the hub.
// ---------------------------------------------------------------------------

// Configure the box sensor pin (ignored when CLIP_SENSOR_TYPE is simulated).
void clip_adherence_begin(uint8_t sensor_pin);

// Set the weekly schedule to track (deep-copied). NULL clears the schedule and
// resets both state machines (e.g. the hub removed this clip).
void clip_adherence_set_schedule(const week_schedule* sched);

// Housekeeping — call from loop().
void clip_adherence_loop(void);

// Next scheduled intake epoch strictly after `now_epoch`, or -1 if there is no
// schedule or no future intake this/next week. Used by the clip's sleep
// scheduler to decide when to wake.
time_t clip_adherence_next_intake(time_t now_epoch);

// True while an intake is in progress (alarm sounding, awaiting the box opening,
// or the box left open) — the clip must stay awake to watch the sensor.
bool clip_adherence_active(void);

#ifdef __cplusplus
}
#endif

#endif // CLIP_ADHERENCE_H
