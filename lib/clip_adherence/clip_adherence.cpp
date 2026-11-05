#include "clip_adherence.h"

#include <Arduino.h>
#include <time.h>
#include <string.h>
#include <limits.h>

#include "clip_alarm.h"
#include "clip_comms.h"

// ---------------------------------------------------------------------------
// Sensor — there is no physical sensor yet. When CLIP_SENSOR_SIMULATED is 1 the
// reading is forced by CLIP_SENSOR_SIM_OPEN (flip it and re-flash to test each
// path). Otherwise the real GPIO is read.
// ---------------------------------------------------------------------------
#define CLIP_SENSOR_SIMULATED   1
#define CLIP_SENSOR_SIM_OPEN    false   // true = box open, false = box closed
#define CLIP_SENSOR_PIN         8       // physical sensor input (simulation off)
#define CLIP_SENSOR_OPEN_LEVEL  LOW     // active level of the physical sensor

// --- tuning ---
#define INTAKE_WINDOW_MINUTES   30      // how long the user has to take the pill
#define BOX_OPEN_GRACE_MS       15000   // wait before flagging a box left open

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

static uint8_t sensor_pin = CLIP_SENSOR_PIN;

static bool have_schedule = false;
static week_schedule schedule;

static box_state_t    box_state    = BOX_CLOSED;
static intake_state_t intake_state = INTAKE_WAITING;

// Epoch (seconds) of the scheduled intake currently being handled, or -1 when
// none. Using epoch instead of week-minute makes each weekly occurrence unique.
static time_t handled_intake_epoch = (time_t)-1;

static uint32_t open_since_ms = 0;

// --- helpers ---

static bool sensor_is_open(void) {
#if CLIP_SENSOR_SIMULATED
    (void)sensor_pin;
    return CLIP_SENSOR_SIM_OPEN;
#else
    return digitalRead(sensor_pin) == CLIP_SENSOR_OPEN_LEVEL;
#endif
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

// --- public API ---

void clip_adherence_begin(uint8_t pin) {
    sensor_pin = pin;
#if !CLIP_SENSOR_SIMULATED
    pinMode(sensor_pin, INPUT_PULLUP);
#endif
    box_state = BOX_CLOSED;
    intake_state = INTAKE_WAITING;
    handled_intake_epoch = (time_t)-1;
}

void clip_adherence_set_schedule(const week_schedule* sched) {
    if (sched == NULL) {
        have_schedule = false;
        memset(&schedule, 0, sizeof(schedule));
        clip_alarm_stop();
        box_state = BOX_CLOSED;
        intake_state = INTAKE_WAITING;
        handled_intake_epoch = (time_t)-1;
        return;
    }
    memcpy(&schedule, sched, sizeof(schedule));
    have_schedule = true;
}

void clip_adherence_loop(void) {
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
        // RTC not set yet — no intake window, but still watch for unexpected opens.
        now_epoch = (time_t)-1;
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
            // A new intake window opened: prompt the user, arm the 30-min timer.
            handled_intake_epoch = active_epoch;
            intake_state = INTAKE_WAITING;
            clip_alarm_start();
            box_state = BOX_WAITING_FOR_OPENING;
            Serial.println("[clip] intake time — waiting for opening");
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
            // 30 minutes elapsed with no opening: missed intake.
            clip_comms_send_missed();
            intake_state = INTAKE_MISSED;
            clip_alarm_stop();
            box_state = BOX_CLOSED;
            Serial.println("[clip] pill missed");
        }
        break;

    case BOX_OPEN:
        if (!sensor_open) {
            box_state = BOX_CLOSED;
            Serial.println("[clip] box closed");
        } else if (ms - open_since_ms >= BOX_OPEN_GRACE_MS) {
            // Still open after the grace period: warn the hub and sound the alarm.
            clip_comms_send_box_open();
            clip_alarm_start();
            box_state = BOX_WAITING_FOR_CLOSING;
            Serial.println("[clip] box left open");
        }
        break;

    case BOX_WAITING_FOR_CLOSING:
        if (!sensor_open) {
            clip_alarm_stop();
            box_state = BOX_CLOSED;
            Serial.println("[clip] box closed");
        }
        // else keep sounding until the box is closed.
        break;
    }
}
