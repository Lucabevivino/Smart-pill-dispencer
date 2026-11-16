#include "clip_adherence.h"

#include <Arduino.h>
#include <time.h>
#include <string.h>
#include <limits.h>
#include <esp_attr.h>   // RTC_DATA_ATTR

#include "clip_alarm.h"
#include "clip_comms.h"

// ---------------------------------------------------------------------------
// Box sensor. Pick ONE backend in CLIP_SENSOR_TYPE. The system ships with the
// reflective IR backend enabled (CLIP_SENSOR_IR_REFLECTIVE); switch CLIP_SENSOR_TYPE
// below if you change the sensor. The sensor pin
// itself is configured in src/Clip/main.cpp (CLIP_ADHERENCE_SENSOR_PIN) and
// passed to clip_adherence_begin(); set it to the ADC pin you wired the sensor to.
//
//   CLIP_SENSOR_SIMULATED        - reading forced to CLIP_SENSOR_SIM_OPEN (flip
//                                  it and re-flash to test each path). No GPIO.
//
//   CLIP_SENSOR_PHOTOTRANSISTOR  - ambient-light sensor on the sensor pin: a
//                                  phototransistor or a photoresistor (LDR) —
//                                  both form the same light-driven voltage
//                                  divider. The box is OPEN when the lid is up
//                                  and light reaches the sensor, i.e. the ADC
//                                  reading rises above the threshold:
//                                  adc > CLIP_SENSOR_LIGHT_THRESHOLD.
//
//   CLIP_SENSOR_IR_REFLECTIVE    - reflective IR sensor (IR LED + IR photo-
//                                  transistor, e.g. TCRT5000) on the sensor pin.
//                                  With the box CLOSED a surface sits in front of
//                                  the sensor and the IR bounces back; OPEN the
//                                  box and nothing reflects, so the return signal
//                                  disappears. The "open" level is derived from a
//                                  stored closed baseline (re-sampled on pairing),
//                                  so it adapts to the box's surface/geometry:
//                                  adc < baseline - CLIP_SENSOR_REFLECT_MARGIN.
//
// Both real backends are read with analogRead(), so the sensor pin must be an
// ADC-capable GPIO (GPIO0..GPIO4 on the ESP32-C3; avoid GPIO2, a strapping pin).
// ---------------------------------------------------------------------------
#define CLIP_SENSOR_SIMULATED       0
#define CLIP_SENSOR_PHOTOTRANSISTOR 1
#define CLIP_SENSOR_IR_REFLECTIVE   2

#define CLIP_SENSOR_TYPE            CLIP_SENSOR_IR_REFLECTIVE

#define CLIP_SENSOR_SIM_OPEN        false   // true = box open, false = box closed

// ADC thresholds (raw 12-bit, 0..4095). Calibrate against your sensor + wiring:
// temporarily set CLIP_SENSOR_DEBUG to 1 to stream the raw value over serial.
// If your particular sensor/wiring inverts the response (light or reflection
// pulls the ADC the other way), flip the comparison in sensor_raw_open().
#define CLIP_SENSOR_LIGHT_THRESHOLD   1500  // phototransistor: open when above
#define CLIP_SENSOR_REFLECT_THRESHOLD 1500  // IR reflective: open when below (fallback)

// For IR reflective, "open" = reading drops this far below the stored closed
// baseline. Re-calibrated on pairing; CLIP_SENSOR_REFLECT_THRESHOLD is only the
// fallback until a baseline has been sampled.
#define CLIP_SENSOR_REFLECT_MARGIN    500

// A raw reading must hold steady this long before it is trusted (debounce). A
// real sensor near its threshold flickers; this keeps the state machine from
// chattering between open/closed.
#define CLIP_SENSOR_DEBOUNCE_MS     20

// Print the raw ADC reading (and resulting open state) every ~500 ms to help
// tune the thresholds above. Set back to 0 once the sensor is calibrated.
#define CLIP_SENSOR_DEBUG           0

// --- tuning ---
#define INTAKE_WINDOW_MINUTES   30      // how long the user has to take the pill
#define BOX_OPEN_GRACE_MS       15000   // wait before flagging a box left open
#define CLIP_ESCALATE_AFTER_SEC 300     // 5 min (design doc) before escalating

#define INTAKE_WINDOW_SECONDS   (INTAKE_WINDOW_MINUTES * 60)

// --- state ---
typedef enum {
    BOX_CLOSED,
    BOX_WAITING_FOR_OPENING,
    BOX_OPEN,
    BOX_WAITING_FOR_CLOSING
} box_state_t;

typedef enum {
    INTAKE_WAITING,
    INTAKE_CONSUMED,
    INTAKE_MISSED
} intake_state_t;

static uint8_t sensor_pin = 0;   // set by clip_adherence_begin()

static bool have_schedule = false;
static week_schedule schedule;

static box_state_t    box_state    = BOX_CLOSED;
static intake_state_t intake_state = INTAKE_WAITING;

// Epoch (seconds) of the scheduled intake currently being handled, or -1 when
// none. Using epoch instead of week-minute makes each weekly occurrence unique.
static time_t handled_intake_epoch = (time_t)-1;

// Millis at which the current intake's prompt (siren) started — drives the
// escalation timer.
static uint32_t intake_armed_ms = 0;

static uint32_t open_since_ms = 0;

// ---------------------------------------------------------------------------
// RTC-persisted adherence state. Survives deep sleep (but not a power cycle,
// where RTC memory is zeroed and magic == 0 means "nothing remembered"):
//   - last_settled_epoch : every scheduled intake at epoch <= this is settled
//     (consumed, missed, or currently being handled) — used to report MISSED
//     for windows the clip slept straight through.
//   - closed_baseline    : IR "closed" reading sampled at the last pairing.
// ---------------------------------------------------------------------------
#define ADHERENCE_RTC_MAGIC 0x41444852u   // "ADHR"

RTC_DATA_ATTR static struct {
    uint32_t magic;
    time_t   last_settled_epoch;
    uint16_t closed_baseline;
    uint8_t  crc;
} adherence_rtc = {0};

static uint8_t rtc_checksum(void) {
    uint8_t sum = 0;
    const uint8_t* p = (const uint8_t*)&adherence_rtc.last_settled_epoch;
    size_t n = (const uint8_t*)&adherence_rtc.crc - p;
    while (n--) {
        sum += *p++;
    }
    return sum;
}

static void rtc_save(void) {
    time_t   settled  = adherence_rtc.last_settled_epoch;
    uint16_t baseline = adherence_rtc.closed_baseline;
    // Re-zero so padding bytes covered by the checksum are deterministic.
    memset(&adherence_rtc, 0, sizeof(adherence_rtc));
    adherence_rtc.last_settled_epoch = settled;
    adherence_rtc.closed_baseline    = baseline;
    adherence_rtc.magic = ADHERENCE_RTC_MAGIC;
    adherence_rtc.crc   = rtc_checksum();
}

// --- helpers ---

// Immediate (raw) sensor reading, mapped to "box is open". No debounce.
static bool sensor_raw_open(void) {
    switch (CLIP_SENSOR_TYPE) {
    case CLIP_SENSOR_PHOTOTRANSISTOR:
        return analogRead(sensor_pin) > CLIP_SENSOR_LIGHT_THRESHOLD;
    case CLIP_SENSOR_IR_REFLECTIVE: {
        int raw = analogRead(sensor_pin);
        int threshold;
        if (adherence_rtc.closed_baseline >= CLIP_SENSOR_REFLECT_MARGIN) {
            // Open when the reading drops below the stored closed baseline.
            threshold = (int)adherence_rtc.closed_baseline - CLIP_SENSOR_REFLECT_MARGIN;
        } else {
            threshold = CLIP_SENSOR_REFLECT_THRESHOLD;   // no baseline yet
        }
        return raw < threshold;
    }
    case CLIP_SENSOR_SIMULATED:
    default:
        return CLIP_SENSOR_SIM_OPEN;
    }
}

// Debounced sensor state — the value the state machine acts on. A raw reading
// only becomes the reported state after it has held steady for
// CLIP_SENSOR_DEBOUNCE_MS, filtering out electrical noise / mechanical bounce.
static bool debounced_open = false;
static bool debounce_candidate = false;
static uint32_t debounce_since_ms = 0;

static bool sensor_is_open(void) {
    bool raw = sensor_raw_open();
    if (raw != debounce_candidate) {
        debounce_candidate = raw;
        debounce_since_ms = millis();
    }
    if (millis() - debounce_since_ms >= CLIP_SENSOR_DEBOUNCE_MS) {
        debounced_open = debounce_candidate;
    }
    return debounced_open;
}

// Read the local clock (set by clip_comms on MSG_ASSIGN) into `out`. Returns
// false until the RTC has a plausible date, in which case no intake window can
// be evaluated yet.
static bool read_clock(hub_time* out, time_t* out_epoch) {
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    if (t == NULL || t->tm_year + 1900 < 2000) {
        return false;
    }
    out->year    = (uint16_t)(t->tm_year + 1900);
    out->mon     = (uint8_t)(t->tm_mon + 1);
    out->mday    = (uint8_t)t->tm_mday;
    out->hour    = (uint8_t)t->tm_hour;
    out->min     = (uint8_t)t->tm_min;
    out->sec     = (uint8_t)t->tm_sec;
    out->weekday = (uint8_t)((t->tm_wday + 6) % 7);   // 0 = Monday
    if (out_epoch != NULL) {
        *out_epoch = now;
    }
    return true;
}

// Epoch of "Monday 00:00:00" of the week that `now` falls in (local time).
static time_t monday_epoch_of(const hub_time* now) {
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = (int)now->year - 1900;
    tm.tm_mon  = (int)now->mon - 1;
    tm.tm_mday = (int)now->mday;
    tm.tm_hour = 0;
    tm.tm_min  = 0;
    tm.tm_sec  = 0;
    tm.tm_isdst = 0;
    time_t midnight = mktime(&tm);
    if (midnight == (time_t)-1) {
        return (time_t)-1;
    }
    int weekday = (int)(now->weekday % 7);   // 0 = Monday ... 6 = Sunday
    return midnight - (time_t)weekday * 86400;
}

// Epoch of the scheduled intake whose 30-minute window currently contains
// `now_epoch`, or -1 if none. Picks the most recent one when several overlap.
static time_t find_active_intake(const hub_time* now, time_t now_epoch) {
    time_t monday = monday_epoch_of(now);
    if (monday == (time_t)-1) {
        return (time_t)-1;
    }

    time_t best = (time_t)-1;
    time_t best_delta = INT_MAX;

    for (int d = 0; d < DAYS_PER_WEEK; d++) {
        const day_schedule* ds = &schedule.days[d];
        for (int i = 0; i < ds->count && i < MAX_INTAKES_PER_DAY; i++) {
            time_t e = monday
                     + (time_t)d * 86400
                     + (time_t)ds->hours[i] * 3600
                     + (time_t)ds->minutes[i] * 60;
            time_t delta = now_epoch - e;
            if (delta >= 0 && delta < INTAKE_WINDOW_SECONDS && delta < best_delta) {
                best_delta = delta;
                best = e;
            }
        }
    }
    return best;
}

// Report MISSED for any scheduled intake whose window fully elapsed while the
// clip was asleep (or otherwise not watching). `last_settled_epoch` bounds how
// far back we look, so each intake is reported at most once even across reboots.
static void catch_up_missed(const hub_time* now, time_t now_epoch) {
    time_t monday = monday_epoch_of(now);
    if (monday == (time_t)-1) {
        return;
    }

    time_t from   = adherence_rtc.last_settled_epoch;
    time_t cutoff = now_epoch - INTAKE_WINDOW_SECONDS;
    if (cutoff <= from) {
        return;   // nothing new to settle
    }

    // Look back two weeks so a multi-day outage is still reconciled.
    for (int week = -2; week <= 0; week++) {
        time_t base = monday + (time_t)week * 7 * 86400;
        for (int d = 0; d < DAYS_PER_WEEK; d++) {
            const day_schedule* ds = &schedule.days[d];
            for (int i = 0; i < ds->count && i < MAX_INTAKES_PER_DAY; i++) {
                time_t e = base
                         + (time_t)d * 86400
                         + (time_t)ds->hours[i] * 3600
                         + (time_t)ds->minutes[i] * 60;
                if (e > from && e <= cutoff) {
                    Serial.printf("[clip] intake missed while asleep (epoch %lld)\n",
                                  (long long)e);
                    clip_comms_send_missed();   // retained if it can't go out yet
                }
            }
        }
    }

    adherence_rtc.last_settled_epoch = cutoff;
    rtc_save();
}

// Mark a freshly detected intake as the one being handled, and record that it is
// settled so catch_up_missed() won't re-report it later.
static void arm_intake(time_t epoch, uint32_t ms) {
    handled_intake_epoch = epoch;
    intake_state = INTAKE_WAITING;
    intake_armed_ms = ms;
    adherence_rtc.last_settled_epoch = epoch;
    rtc_save();
}

// Re-sample the IR "closed" baseline (the box should be closed during pairing).
static void recalibrate_baseline(void) {
    if (CLIP_SENSOR_TYPE != CLIP_SENSOR_IR_REFLECTIVE) {
        return;
    }
    long sum = 0;
    for (int i = 0; i < 16; i++) {
        sum += analogRead(sensor_pin);
        delay(5);
    }
    adherence_rtc.closed_baseline = (uint16_t)(sum / 16);
    rtc_save();
    Serial.printf("[clip] IR closed baseline = %u\n", adherence_rtc.closed_baseline);
}

// --- public API ---

void clip_adherence_begin(uint8_t pin) {
    sensor_pin = pin;
    if (CLIP_SENSOR_TYPE != CLIP_SENSOR_SIMULATED) {
        analogReadResolution(12);   // thresholds above are expressed on 0..4095
        pinMode(sensor_pin, INPUT); // ADC; analogRead() configures the channel
    }

    // Reset the debounce filter so a stale reading from a previous boot (or a
    // re-begin() after a deep-sleep wake) doesn't survive into this run.
    debounced_open = false;
    debounce_candidate = false;
    debounce_since_ms = 0;

    box_state = BOX_CLOSED;
    intake_state = INTAKE_WAITING;
    handled_intake_epoch = (time_t)-1;
    intake_armed_ms = 0;
    open_since_ms = 0;

    // Restore (or discard) the RTC-persisted settled epoch + IR baseline.
    if (adherence_rtc.magic != ADHERENCE_RTC_MAGIC ||
        adherence_rtc.crc != rtc_checksum()) {
        memset(&adherence_rtc, 0, sizeof(adherence_rtc));
    }
}

void clip_adherence_set_schedule(const week_schedule* sched) {
    // Reset both state machines on every (re)application, so an interrupted
    // intake or a stale handled epoch can't leak into the new schedule.
    clip_alarm_stop();
    box_state = BOX_CLOSED;
    intake_state = INTAKE_WAITING;
    handled_intake_epoch = (time_t)-1;
    intake_armed_ms = 0;
    open_since_ms = 0;

    if (sched == NULL) {
        have_schedule = false;
        memset(&schedule, 0, sizeof(schedule));
        return;
    }
    memcpy(&schedule, sched, sizeof(schedule));
    have_schedule = true;
}

void clip_adherence_on_assign(void) {
    // New pairing: nothing before now is relevant for this medicine.
    time_t now = time(NULL);
    adherence_rtc.last_settled_epoch = (now > 0) ? now : 0;
    rtc_save();
    recalibrate_baseline();
}

void clip_adherence_loop(void) {
#if CLIP_SENSOR_DEBUG
    // Stream the raw ADC value unconditionally so the sensor can be tested and
    // calibrated without a hub or a schedule (the state machine below only runs
    // once a schedule is assigned).
    if (CLIP_SENSOR_TYPE != CLIP_SENSOR_SIMULATED) {
        static uint32_t last_sensor_debug_ms = 0;
        uint32_t now_ms = millis();
        if (now_ms - last_sensor_debug_ms >= 500) {
            last_sensor_debug_ms = now_ms;
            Serial.printf("[clip] sensor raw=%d\n", analogRead(sensor_pin));
        }
    }
#endif

    if (!have_schedule) {
        clip_alarm_stop();
        box_state = BOX_CLOSED;
        intake_state = INTAKE_WAITING;
        handled_intake_epoch = (time_t)-1;
        return;
    }

    hub_time now;
    time_t now_epoch;
    if (!read_clock(&now, &now_epoch)) {
        now_epoch = (time_t)-1;   // RTC not set yet — can't evaluate windows
    }

    // 1) Report any intake whose window the clip slept through, before arming
    //    the next one.
    if (now_epoch != (time_t)-1) {
        catch_up_missed(&now, now_epoch);
    }

    time_t active_epoch = (now_epoch == (time_t)-1)
                              ? (time_t)-1
                              : find_active_intake(&now, now_epoch);

    bool sensor_open = sensor_is_open();
    uint32_t ms = millis();

    switch (box_state) {
    case BOX_CLOSED:
        clip_alarm_stop();
        if (active_epoch != (time_t)-1 && active_epoch != handled_intake_epoch) {
            // A fresh intake window opened: prompt the user and arm the timer.
            arm_intake(active_epoch, ms);
            clip_alarm_start();
            box_state = BOX_WAITING_FOR_OPENING;
            Serial.println("[clip] intake time — waiting for opening");
        } else if (handled_intake_epoch != (time_t)-1 &&
                   intake_state == INTAKE_WAITING &&
                   active_epoch == handled_intake_epoch) {
            // The box just closed after an intake was detected while it was open:
            // prompt for it now that the opening can actually be detected.
            intake_armed_ms = ms;
            clip_alarm_start();
            box_state = BOX_WAITING_FOR_OPENING;
            Serial.println("[clip] box closed — prompting pending intake");
        } else if (sensor_open) {
            // Unexpected open (no active intake window) — start the grace timer.
            open_since_ms = ms;
            box_state = BOX_OPEN;
            Serial.println("[clip] unexpected open");
        }
        break;

    case BOX_WAITING_FOR_OPENING:
        if (sensor_open) {
            // Pill taken: report it immediately, then wait for the box to close.
            clip_comms_send_taken();
            intake_state = INTAKE_CONSUMED;
            clip_alarm_stop();
            open_since_ms = ms;
            box_state = BOX_OPEN;
            Serial.println("[clip] pill taken");
        } else if (now_epoch != (time_t)-1 &&
                   now_epoch - handled_intake_epoch >= INTAKE_WINDOW_SECONDS) {
            // Window expired with no opening: missed intake.
            clip_comms_send_missed();
            intake_state = INTAKE_MISSED;
            clip_alarm_stop();
            box_state = BOX_CLOSED;
            Serial.println("[clip] pill missed");
        } else if (ms - intake_armed_ms >= (uint32_t)CLIP_ESCALATE_AFTER_SEC * 1000UL) {
            // 5 minutes unheeded: escalate to the more urgent pattern (idempotent).
            clip_alarm_escalate();
        }
        break;

    case BOX_OPEN:
        if (!sensor_open) {
            box_state = BOX_CLOSED;
            Serial.println("[clip] box closed");
            break;
        }
        // A new intake may open (or a pending one expire) while the box is open.
        if (active_epoch != (time_t)-1 && active_epoch != handled_intake_epoch) {
            arm_intake(active_epoch, ms);   // prompt only once the box is closed
            Serial.println("[clip] intake pending (box open)");
        } else if (handled_intake_epoch != (time_t)-1 &&
                   intake_state == INTAKE_WAITING &&
                   now_epoch != (time_t)-1 &&
                   now_epoch - handled_intake_epoch >= INTAKE_WINDOW_SECONDS) {
            clip_comms_send_missed();
            intake_state = INTAKE_MISSED;
            Serial.println("[clip] intake missed (box left open)");
        }
        if (ms - open_since_ms >= BOX_OPEN_GRACE_MS) {
            // Still open after the grace period: warn the hub and remind the user.
            clip_comms_send_box_open();
            clip_alarm_remind();
            box_state = BOX_WAITING_FOR_CLOSING;
            Serial.println("[clip] box left open");
        }
        break;

    case BOX_WAITING_FOR_CLOSING:
        if (!sensor_open) {
            clip_alarm_stop();
            box_state = BOX_CLOSED;
            Serial.println("[clip] box closed");
            break;
        }
        clip_alarm_remind();   // idempotent
        // Same pending-intake / expiry handling as BOX_OPEN.
        if (active_epoch != (time_t)-1 && active_epoch != handled_intake_epoch) {
            arm_intake(active_epoch, ms);
            Serial.println("[clip] intake pending (box open)");
        } else if (handled_intake_epoch != (time_t)-1 &&
                   intake_state == INTAKE_WAITING &&
                   now_epoch != (time_t)-1 &&
                   now_epoch - handled_intake_epoch >= INTAKE_WINDOW_SECONDS) {
            clip_comms_send_missed();
            intake_state = INTAKE_MISSED;
            Serial.println("[clip] intake missed (box left open)");
        }
        break;
    }
}

// Epoch of "Monday 00:00:00" of the week containing `now_epoch` (local time).
// Mirrors monday_epoch_of() above but derives the week from an epoch directly.
static time_t monday_epoch_from_epoch(time_t now_epoch) {
    struct tm* t = localtime(&now_epoch);
    if (t == NULL) {
        return (time_t)-1;
    }
    struct tm tm = *t;
    int weekday = (tm.tm_wday + 6) % 7;   // 0 = Monday
    tm.tm_hour = 0;
    tm.tm_min  = 0;
    tm.tm_sec  = 0;
    tm.tm_isdst = 0;
    time_t midnight = mktime(&tm);
    if (midnight == (time_t)-1) {
        return (time_t)-1;
    }
    return midnight - (time_t)weekday * 86400;
}

time_t clip_adherence_next_intake(time_t now_epoch) {
    if (!have_schedule) {
        return (time_t)-1;
    }
    time_t monday = monday_epoch_from_epoch(now_epoch);
    if (monday == (time_t)-1) {
        return (time_t)-1;
    }

    time_t best = (time_t)-1;
    for (int week = 0; week < 2; week++) {   // this week, then next (wrap)
        time_t base = monday + (time_t)week * 7 * 86400;
        for (int d = 0; d < DAYS_PER_WEEK; d++) {
            const day_schedule* ds = &schedule.days[d];
            for (int i = 0; i < ds->count && i < MAX_INTAKES_PER_DAY; i++) {
                time_t e = base
                         + (time_t)d * 86400
                         + (time_t)ds->hours[i] * 3600
                         + (time_t)ds->minutes[i] * 60;
                if (e > now_epoch && (best == (time_t)-1 || e < best)) {
                    best = e;
                }
            }
        }
        if (best != (time_t)-1) {
            break;   // the earliest future intake is in the first week that has one
        }
    }
    return best;
}

bool clip_adherence_active(void) {
    return box_state != BOX_CLOSED;
}
