// ---------------------------------------------------------------------------
// Photodiode box-sensor bench — Arduino Uno version.
//
// A direct port of the ESP32-C3 `bench-photodiode` firmware, adapted for the
// ATmega328P: 10-bit ADC (0..1023) instead of 12-bit, no printf (built from
// Serial.print), and no USB-CDC (the Uno resets when the Serial Monitor opens,
// which is fine here).
//
// Reads ONE analog pin at a high rate and streams the raw value plus rolling
// stats, so the open-vs-closed separation can be measured and a threshold
// chosen. Output is the same CSV as the ESP32 version:
//     millis,raw,state,min,max,mean,cycles,threshold
//       raw       0..1023 ADC reading (10-bit)
//       state     1 = open (raw > threshold), 0 = closed
//       min/max/mean  rolling over the last WINDOW_SIZE samples
//       cycles    number of completed open->closed transitions
//       threshold the open/closed decision level active for this sample
//
// Send "t=<n>" over the Serial Monitor to change the threshold, e.g. "t=512".
// ---------------------------------------------------------------------------

#include <string.h>
#include <stdlib.h>

#define SENSOR_PIN         A0    // photodiode divider midpoint -> A0 (analog input)
#define OPEN_THRESHOLD     512   // initial threshold, 0..1023
#define SAMPLE_INTERVAL_MS 100
#define WINDOW_SIZE        64

static int threshold = OPEN_THRESHOLD;

static int window[WINDOW_SIZE];
static int window_head = 0;
static int window_count = 0;

static bool last_open = false;
static unsigned long cycles = 0;
static unsigned long last_sample_ms = 0;

static void handle_command(const char* cmd) {
    if (strncmp(cmd, "t=", 2) == 0) {
        long v = strtol(cmd + 2, NULL, 10);
        if (v >= 0 && v <= 1023) {
            threshold = (int)v;
            Serial.print("# threshold set to ");
            Serial.println(threshold);
        } else {
            Serial.println("# threshold must be 0..1023");
        }
    } else {
        Serial.println("# unknown command (use t=<n>)");
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);   // let the Serial Monitor attach

    Serial.println("# PHOTODIODE_BENCH_BEGIN");
    Serial.print("# pin=A0 threshold="); Serial.print(threshold);
    Serial.print(" interval="); Serial.print(SAMPLE_INTERVAL_MS);
    Serial.print("ms window="); Serial.println(WINDOW_SIZE);
    Serial.println("# format: millis,raw,state,min,max,mean,cycles,threshold");
    Serial.println("# commands: t=<n> sets the open threshold");
}

void loop() {
    // Drain serial commands (non-blocking).
    static char cmd[32];
    static int ci = 0;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (ci > 0) {
                cmd[ci] = '\0';
                handle_command(cmd);
                ci = 0;
            }
        } else if (ci < (int)sizeof(cmd) - 1) {
            cmd[ci++] = c;
        }
    }

    unsigned long now = millis();
    if (now - last_sample_ms < SAMPLE_INTERVAL_MS) {
        return;
    }
    last_sample_ms = now;

    int raw = analogRead(SENSOR_PIN);
    bool open = (raw > threshold);

    window[window_head] = raw;
    window_head = (window_head + 1) % WINDOW_SIZE;
    if (window_count < WINDOW_SIZE) {
        window_count++;
    }

    if (last_open && !open) {
        cycles++;
    }
    last_open = open;

    int mn = 1023, mx = 0;
    long sum = 0;
    for (int i = 0; i < window_count; i++) {
        int v = window[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        sum += v;
    }
    int mean = (window_count > 0) ? (int)(sum / window_count) : 0;

    Serial.print(now); Serial.print(',');
    Serial.print(raw); Serial.print(',');
    Serial.print(open ? 1 : 0); Serial.print(',');
    Serial.print(mn); Serial.print(',');
    Serial.print(mx); Serial.print(',');
    Serial.print(mean); Serial.print(',');
    Serial.print(cycles); Serial.print(',');
    Serial.println(threshold);
}
