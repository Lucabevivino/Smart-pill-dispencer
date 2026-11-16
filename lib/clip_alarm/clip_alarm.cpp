#include "clip_alarm.h"

#include <Arduino.h>

// --- tuning ---
#define SIREN_HIGH_HZ          950.0   // two-tone ambulance siren
#define SIREN_LOW_HZ           650.0
#define SIREN_HALF_MS          400     // switch tone every 400 ms (siren)
#define URGENT_HALF_MS         150     // faster tone switch (escalate)
#define CHIRP_ON_MS            150     // reminder: short beep…
#define CHIRP_OFF_MS           1500    // …mostly quiet
#define LED_BLINK_MS           250     // LED on/off period (siren + escalate)
#define LEDC_CHANNEL           0
#define LEDC_RESOLUTION_BITS   8

// LED polarity — swap these two if your LED is active-low (lights when LOW).
#define LED_ON  HIGH
#define LED_OFF LOW

typedef enum {
    PAT_SIREN,
    PAT_URGENT,
    PAT_CHIRP
} pattern_t;

static uint8_t led_pin = 0;
static uint8_t buzzer_pin = 0;

static bool alerting = false;
static bool output_on = false;

static pattern_t pattern = PAT_SIREN;
static bool siren_high = true;       // which tone of the two-tone siren
static bool chirp_on = true;         // whether the chirp is in its "on" phase
static bool led_on = false;
static uint32_t last_tone_toggle_ms = 0;
static uint32_t last_led_toggle_ms = 0;

// Identify-blink (hub MSG_BLINK). Only drives the LED while no pattern is active.
static uint32_t blink_until_ms = 0;        // 0 = no blink pending
static uint32_t last_blink_toggle_ms = 0;
static bool blink_on = false;

// --- output ---

static void pattern_begin(pattern_t p) {
    pattern = p;
    output_on = true;
    alerting = true;

    siren_high = true;
    chirp_on = true;
    led_on = true;
    last_tone_toggle_ms = millis();
    last_led_toggle_ms = millis();

    digitalWrite(led_pin, LED_ON);
    ledcWriteTone(LEDC_CHANNEL, (p == PAT_CHIRP) ? SIREN_LOW_HZ : SIREN_HIGH_HZ);
}

static void alert_off(void) {
    output_on = false;
    alerting = false;
    ledcWrite(LEDC_CHANNEL, 0);   // silence
    digitalWrite(led_pin, LED_OFF);
}

static void alert_update(void) {
    if (!output_on) {
        return;
    }
    uint32_t now = millis();

    if (pattern == PAT_CHIRP) {
        // Gentle reminder: a short low beep, then mostly quiet.
        if (chirp_on) {
            if (now - last_tone_toggle_ms >= CHIRP_ON_MS) {
                chirp_on = false;
                last_tone_toggle_ms = now;
                ledcWrite(LEDC_CHANNEL, 0);
                digitalWrite(led_pin, LED_OFF);
            }
        } else if (now - last_tone_toggle_ms >= CHIRP_OFF_MS) {
            chirp_on = true;
            last_tone_toggle_ms = now;
            ledcWriteTone(LEDC_CHANNEL, SIREN_LOW_HZ);
            digitalWrite(led_pin, LED_ON);
        }
    } else {
        uint32_t half = (pattern == PAT_URGENT) ? URGENT_HALF_MS : SIREN_HALF_MS;
        if (now - last_tone_toggle_ms >= half) {
            last_tone_toggle_ms = now;
            siren_high = !siren_high;
            ledcWriteTone(LEDC_CHANNEL, siren_high ? SIREN_HIGH_HZ : SIREN_LOW_HZ);
        }
        if (now - last_led_toggle_ms >= LED_BLINK_MS) {
            last_led_toggle_ms = now;
            led_on = !led_on;
            digitalWrite(led_pin, led_on ? LED_ON : LED_OFF);
        }
    }
}

// --- public API ---

void clip_alarm_begin(uint8_t led, uint8_t buzzer) {
    led_pin = led;
    buzzer_pin = buzzer;

    pinMode(led_pin, OUTPUT);
    digitalWrite(led_pin, LED_OFF);

    ledcSetup(LEDC_CHANNEL, (double)SIREN_HIGH_HZ, LEDC_RESOLUTION_BITS);
    ledcAttachPin(buzzer_pin, LEDC_CHANNEL);
    ledcWrite(LEDC_CHANNEL, 0);   // start silent
}

void clip_alarm_start(void) {
    if (output_on && pattern == PAT_SIREN) {
        return;
    }
    pattern_begin(PAT_SIREN);
    Serial.println("[clip] alert on (siren)");
}

void clip_alarm_escalate(void) {
    if (output_on && pattern == PAT_URGENT) {
        return;
    }
    pattern_begin(PAT_URGENT);
    Serial.println("[clip] alert escalated");
}

void clip_alarm_remind(void) {
    if (output_on && pattern == PAT_CHIRP) {
        return;
    }
    pattern_begin(PAT_CHIRP);
    Serial.println("[clip] close-box reminder on");
}

void clip_alarm_stop(void) {
    if (!alerting) {
        return;
    }
    alert_off();
    Serial.println("[clip] alert off");
}

bool clip_alarm_is_alerting(void) {
    return alerting;
}

void clip_alarm_loop(void) {
    // Identify-blink (hub MSG_BLINK). Only drives the LED while no pattern is
    // sounding, so the two never fight over the pin.
    if (blink_until_ms != 0 && !alerting) {
        uint32_t now = millis();
        if (now < blink_until_ms) {
            if (now - last_blink_toggle_ms >= LED_BLINK_MS) {
                last_blink_toggle_ms = now;
                blink_on = !blink_on;
                digitalWrite(led_pin, blink_on ? LED_ON : LED_OFF);
            }
        } else {
            blink_until_ms = 0;
            digitalWrite(led_pin, LED_OFF);
        }
    }

    alert_update();
}

void clip_alarm_self_test(void) {
    // LED: three quick blinks.
    for (int i = 0; i < 3; i++) {
        digitalWrite(led_pin, LED_ON);
        delay(150);
        digitalWrite(led_pin, LED_OFF);
        delay(150);
    }
    // Buzzer: one short beep.
    ledcWriteTone(LEDC_CHANNEL, SIREN_HIGH_HZ);
    delay(300);
    ledcWrite(LEDC_CHANNEL, 0);
    Serial.println("[clip] self-test done (LED 3x + buzzer 1x)");
}

void clip_alarm_blink(uint8_t seconds) {
    if (seconds == 0) {
        return;
    }
    blink_until_ms = millis() + (uint32_t)seconds * 1000UL;
    blink_on = true;
    digitalWrite(led_pin, LED_ON);
    last_blink_toggle_ms = millis();
    Serial.print("[clip] blink for ");
    Serial.print((int)seconds);
    Serial.println(" s");
}
