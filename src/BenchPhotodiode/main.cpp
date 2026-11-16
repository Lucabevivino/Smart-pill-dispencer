// ---------------------------------------------------------------------------
// Photodiode box-sensor bench — standalone test firmware for the ambient-light
// phototransistor / photodiode used as the box open/closed sensor on the clip.
//
// Purpose: read two ADC pins at a high rate and stream both raw values plus
// rolling stats, so the open-vs-closed separation can be measured and a
// reliable threshold chosen (then copied into CLIP_SENSOR_LIGHT_THRESHOLD in
// lib/clip_adherence/clip_adherence.cpp). Deliberately self-contained: it does
// NOT include any project library (no ESP-NOW, no alarm, no schedule, no deep
// sleep), so the reading is isolated from everything else on the clip.
//
// Build & flash:
//   pio run -e bench-photodiode -t upload
//   pio run -e bench-photodiode -t monitor
//
// Serial output is one CSV line per sample:
//   millis,raw0,raw4,delta,state,min,max,mean,cycles,threshold
//     raw0   0..4095 ADC reading (12-bit) of the box-interior photodiode
//     raw4   0..4095 ADC reading (12-bit) of the ambient photodiode
//     delta  raw4 - raw0 (ambient minus box)
//     state  1 = open (raw0 > threshold), 0 = closed
//     min/max/mean  rolling over the last WINDOW_SIZE samples of raw0
//     cycles number of completed open->closed transitions so far
//     threshold  the open/closed decision level active for this sample
//
// Send "t=<n>" over serial to change the threshold at runtime, e.g. "t=1200".
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

// --- wiring / tuning ---
// Two photodiodes in identical dividers: one inside the box (open/closed),
// one outside (ambient light reference). The delta (ambient - box) is the
// signal for an adaptive threshold: open => both see ~the same light (delta
// near 0), closed => the box diode goes dark while the ambient one keeps
// tracking, so |delta| grows.
#define BOX_SENSOR_PIN      0     // box-interior photodiode (open/closed), ADC1_CH0
#define AMBIENT_SENSOR_PIN  4     // external photodiode (ambient reference), ADC1_CH4
#define OPEN_THRESHOLD      1500  // initial threshold; adjust at runtime with "t=<n>"
#define SAMPLE_INTERVAL_MS  100   // one sample every 100 ms
#define WINDOW_SIZE         64    // rolling-stats window (samples)

static uint16_t threshold = OPEN_THRESHOLD;

static uint16_t window[WINDOW_SIZE];
static uint16_t window_head = 0;   // next write index
static uint16_t window_count = 0;  // valid entries (up to WINDOW_SIZE)

static bool last_open = false;
static uint32_t cycles = 0;        // completed open->closed transitions
static uint32_t last_sample_ms = 0;

// Handle a serial command line (terminated by \n or \r). Currently only "t=<n>".
static void handle_command(const char* cmd) {
    if (strncmp(cmd, "t=", 2) == 0) {
        long v = strtol(cmd + 2, NULL, 10);
        if (v >= 0 && v <= 4095) {
            threshold = (uint16_t)v;
            Serial.printf("# threshold set to %u\n", threshold);
        } else {
            Serial.println("# threshold must be 0..4095");
        }
    } else {
        Serial.println("# unknown command (use t=<n>)");
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);   // let the native USB CDC port enumerate before the first print

    analogReadResolution(12);
    pinMode(BOX_SENSOR_PIN, INPUT);
    pinMode(AMBIENT_SENSOR_PIN, INPUT);

    Serial.println("# PHOTODIODE_BENCH_BEGIN");
    Serial.printf("# box_pin=%d ambient_pin=%d threshold=%u interval=%ums window=%u\n",
                  BOX_SENSOR_PIN, AMBIENT_SENSOR_PIN, threshold,
                  SAMPLE_INTERVAL_MS, WINDOW_SIZE);
    Serial.println("# format: millis,raw0,raw4,delta,state,min,max,mean,cycles,threshold");
    Serial.println("# delta = raw4 - raw0 (ambient minus box)");
    Serial.println("# commands: t=<n> sets the open threshold");
}

void loop() {
    // Drain serial commands (non-blocking).
    static char cmd[32];
    static uint8_t ci = 0;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (ci > 0) {
                cmd[ci] = '\0';
                handle_command(cmd);
                ci = 0;
            }
        } else if (ci < sizeof(cmd) - 1) {
            cmd[ci++] = c;
        }
    }

    uint32_t now = millis();
    if (now - last_sample_ms < SAMPLE_INTERVAL_MS) {
        return;
    }
    last_sample_ms = now;

    int raw0 = analogRead(BOX_SENSOR_PIN);
    int raw4 = analogRead(AMBIENT_SENSOR_PIN);
    int delta = raw4 - raw0;
    bool open = (raw0 > (int)threshold);

    // Rolling window (on the box sensor).
    window[window_head] = (uint16_t)raw0;
    window_head = (window_head + 1) % WINDOW_SIZE;
    if (window_count < WINDOW_SIZE) {
        window_count++;
    }

    // Completed open->closed transitions.
    if (last_open && !open) {
        cycles++;
    }
    last_open = open;

    int mn = 4095, mx = 0;
    long sum = 0;
    for (uint16_t i = 0; i < window_count; i++) {
        uint16_t v = window[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        sum += v;
    }
    int mean = (window_count > 0) ? (int)(sum / window_count) : 0;

    Serial.printf("%lu,%d,%d,%d,%d,%d,%d,%d,%lu,%u\n",
                  (unsigned long)now, raw0, raw4, delta, open ? 1 : 0,
                  mn, mx, mean, (unsigned long)cycles, (unsigned)threshold);
}
