#include <Arduino.h>
#include "clip_comms.h"
#include "clip_alarm.h"
#include "clip_adherence.h"

// ---------------------------------------------------------------------------
// Alert hardware (TEMPORARY — set these to your actual wiring).
//   LED    -> CLIP_ALARM_LED_PIN     (active-high by default; see clip_alarm.cpp)
//   Buzzer -> CLIP_ALARM_BUZZER_PIN  (passive buzzer, driven by LEDC PWM)
// ---------------------------------------------------------------------------
#define CLIP_ALARM_LED_PIN     5
#define CLIP_ALARM_BUZZER_PIN  7

// Box sensor input. Only used when the simulated sensor is disabled
// (see CLIP_SENSOR_SIMULATED in clip_adherence.cpp).
#define CLIP_ADHERENCE_SENSOR_PIN 8

static bool schedule_loaded = false;

void setup() {
    Serial.begin(115200);

    clip_comms_begin();                                  // ESP-NOW + restore persisted hub
    clip_alarm_begin(CLIP_ALARM_LED_PIN, CLIP_ALARM_BUZZER_PIN);
    clip_alarm_self_test();                              // LED 3x + buzzer 1x (hardware check)
    clip_adherence_begin(CLIP_ADHERENCE_SENSOR_PIN);     // box sensor + state machines

    if (clip_comms_find_hub(30000)) {                    // scan (skips if remembered)
        clip_comms_register(30000);                      // HELLO + wait for the hub's ACK
    }
}

void loop() {
    clip_comms_loop();                                   // drains MSG_ASSIGN -> RTC + schedule

    // Once assigned, hand the schedule to the adherence machine (once). If the hub
    // removes us later, clear the schedule and silence the clip.
    if (!schedule_loaded && clip_comms_is_assigned()) {
        clip_adherence_set_schedule(clip_comms_schedule());
        schedule_loaded = true;
    } else if (schedule_loaded && !clip_comms_is_assigned()) {
        clip_adherence_set_schedule(NULL);
        schedule_loaded = false;
    }

    // Hub asked this clip to blink (identify the pill box).
    uint8_t blink_sec;
    if (clip_comms_take_blink(&blink_sec)) {
        clip_alarm_blink(blink_sec);
    }

    clip_adherence_loop();                               // box + intake state machines
    clip_alarm_loop();                                   // drives the LED/buzzer pattern
}
