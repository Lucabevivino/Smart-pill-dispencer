#include <Arduino.h>
#include <time.h>
#include <esp_sleep.h>
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

// Box sensor input. How it is read (and what "open" means) is selected by
// CLIP_SENSOR_TYPE in clip_adherence.cpp:
//   - phototransistor: ambient-light phototransistor — open when light is seen
//   - IR reflective:   reflective IR sensor (TCRT5000-style) — open when nothing
//                       reflects back
// Both real backends use analogRead(), so this pin must be ADC-capable
// (GPIO0..GPIO4 on the ESP32-C3; avoid GPIO2, a strapping pin). The simulated
// backend ignores this pin entirely.
#define CLIP_ADHERENCE_SENSOR_PIN 0

// ---------------------------------------------------------------------------
// Deep sleep. When enabled the clip sleeps between events and wakes for the next
// scheduled intake or the next re-sync (CLIP_RESYNC_INTERVAL_SEC, defined in
// clip_comms.h), whichever is sooner. Keep it off while developing so the clip
// stays awake and re-syncs in place (easier to observe on serial).
// ---------------------------------------------------------------------------
#define DEEP_SLEEP_ON 1

// Don't bother sleeping for less than this — the wake overhead isn't worth it.
#define CLIP_MIN_SLEEP_SEC 1

// How often to retry a failed re-sync while the hub is unreachable.
#define CLIP_RESYNC_RETRY_MS 30000

// How often to retry a full hub scan while we have no hub. Unassigned clips stay
// awake, so this keeps looking until the hub appears.
#define CLIP_RESCAN_INTERVAL_MS 1000

static bool schedule_loaded = false;

void setup() {
    Serial.begin(115200);

    // A timer wake is a full reboot, so setup() runs again. Remember why we woke
    // so we can skip the loud boot self-test on a quiet re-sync wake.
    esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();

    clip_comms_begin();                                  // ESP-NOW + restore persisted hub
    clip_alarm_begin(CLIP_ALARM_LED_PIN, CLIP_ALARM_BUZZER_PIN);

    // Hardware self-test only on a cold boot — don't blink/beep the box on every
    // re-sync wake.
    if (wake != ESP_SLEEP_WAKEUP_TIMER) {
        clip_alarm_self_test();
    }

    clip_adherence_begin(CLIP_ADHERENCE_SENSOR_PIN);     // box sensor + state machines

    // Re-discover / register. On a deep-sleep wake the hub is remembered (see
    // clip_comms_begin), so find_hub returns immediately; re-registering keeps the
    // hub from pruning us as stale. If that remembered hub no longer answers, drop
    // it so the loop() re-scan can find the real one (it may have changed channel).
    if (clip_comms_find_hub(30000) && !clip_comms_is_registered()) {
        if (!clip_comms_register(30000)) {
            clip_comms_forget_hub();
        }
    }
}

// Re-anchor the RTC once CLIP_RESYNC_INTERVAL_SEC has elapsed. Runs whether or
// not deep sleep is on, so the re-sync path is testable without sleeping.
static void clip_maybe_resync(void) {
    time_t now = time(NULL);
    if (!clip_comms_is_assigned() || !clip_comms_resync_due(now)) {
        return;
    }

    // Throttle attempts so an unreachable hub doesn't block the loop every pass.
    static uint32_t last_attempt_ms = 0;
    if (millis() - last_attempt_ms < CLIP_RESYNC_RETRY_MS) {
        return;
    }
    last_attempt_ms = millis();
    clip_comms_request_time_sync(5000);
}

// If we have no hub (never found one, or forgot a stale one), keep scanning until
// we do. find_hub() blocks while it hops channels, so rate-limit the attempts.
static void clip_maybe_rediscover(void) {
    if (clip_comms_is_connected()) {
        return;
    }
    static uint32_t last_scan_ms = 0;
    if (millis() - last_scan_ms < CLIP_RESCAN_INTERVAL_MS) {
        return;
    }
    last_scan_ms = millis();
    if (clip_comms_find_hub(30000) && !clip_comms_is_registered()) {
        clip_comms_register(30000);
    }
}

// Sleep until the next intake or the next re-sync, whichever comes first. Only
// meaningful once assigned with a valid clock and no intake in progress.
static void clip_maybe_sleep(void) {
#if DEEP_SLEEP_ON
    if (!clip_comms_is_assigned()) {
        return;   // unassigned: stay awake so the hub can discover us
    }
    time_t now = time(NULL);
    if (now < (time_t)1000000000) {
        return;   // clock not synced yet — can't schedule a wake
    }
    if (clip_adherence_active()) {
        return;   // intake in progress: keep watching the sensor
    }

    time_t next_intake = clip_adherence_next_intake(now);   // -1 if none
    time_t next_resync = clip_comms_last_resync_epoch() + CLIP_RESYNC_INTERVAL_SEC;
    time_t wake_at = (next_intake == (time_t)-1) ? next_resync
                     : (next_intake < next_resync ? next_intake : next_resync);

    if (wake_at <= now) {
        return;   // something is due right now — handle it, then sleep next pass
    }
    uint64_t secs = (uint64_t)(wake_at - now);
    if (secs < CLIP_MIN_SLEEP_SEC) {
        return;
    }

    // Give queued adherence events a moment to flush (and be ACKed) before
    // sleeping. They're RTC-backed anyway, so a miss only delays them one wake.
    uint32_t flush_start = millis();
    while (millis() - flush_start < 1500) {
        clip_comms_loop();
    }

    Serial.printf("[clip] sleeping %llu s (wake at %lld)\n",
                  (unsigned long long)secs, (long long)wake_at);
    esp_sleep_enable_timer_wakeup(secs * 1000000ULL);
    esp_deep_sleep_start();
#endif
}

void loop() {
    clip_comms_loop();                                   // drains MSG_ASSIGN -> RTC + schedule
    clip_maybe_rediscover();                             // keep scanning if no hub yet

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

    clip_maybe_resync();
    clip_maybe_sleep();
}
