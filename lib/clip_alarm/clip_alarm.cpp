#include "clip_alarm.h"

#include <Arduino.h>

// --- tuning ---
#define SIREN_HIGH_HZ          950.0   // two-tone ambulance siren
#define SIREN_LOW_HZ           650.0
#define SIREN_HALF_MS          400     // switch tone every 400 ms
#define LED_BLINK_MS           250     // LED on/off period
#define LEDC_CHANNEL           0
#define LEDC_RESOLUTION_BITS   8

// LED polarity — swap these two if your LED is active-low (lights when LOW).
#define LED_ON  HIGH
#define LED_OFF LOW

static uint8_t led_pin = 0;
static uint8_t buzzer_pin = 0;

static bool alerting = false;
static bool output_on = false;

static bool siren_high = true;
static bool led_on = false;
static uint32_t last_siren_toggle_ms = 0;
static uint32_t last_led_toggle_ms = 0;

// Identify-blink (hub MSG_BLINK). Only drives the LED while the alarm is quiet.
static uint32_t blink_until_ms = 0;        // 0 = no blink pending
static uint32_t last_blink_toggle_ms = 0;
static bool blink_on = false;

// --- output ---

static void alert_on(void) {
    if (output_on) {
        return;
    }
    output_on = true;
    siren_high = true;
    led_on = true;
    ledcWriteTone(LEDC_CHANNEL, SIREN_HIGH_HZ);
    digitalWrite(led_pin, LED_ON);
    last_siren_toggle_ms = millis();
    last_led_toggle_ms = millis();
}

static void alert_off(void) {
    if (!output_on) {
        return;
    }
    output_on = false;
    ledcWrite(LEDC_CHANNEL, 0);   // silence
    digitalWrite(led_pin, LED_OFF);
}

static void alert_update(void) {
    if (!output_on) {
        return;
    }
    uint32_t now = millis();

    if (now - last_siren_toggle_ms >= SIREN_HALF_MS) {
        last_siren_toggle_ms = now;
        siren_high = !siren_high;
        ledcWriteTone(LEDC_CHANNEL, siren_high ? SIREN_HIGH_HZ : SIREN_LOW_HZ);
    }
    if (now - last_led_toggle_ms >= LED_BLINK_MS) {
        last_led_toggle_ms = now;
        led_on = !led_on;
        digitalWrite(led_pin, led_on ? LED_ON : LED_OFF);
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
    if (alerting) {
        return;
    }
    alerting = true;
    alert_on();
    Serial.println("[clip] alert on");
}

void clip_alarm_stop(void) {
    if (!alerting) {
        return;
    }
    alerting = false;
    alert_off();
    Serial.println("[clip] alert off");
}

bool clip_alarm_is_alerting(void) {
    return alerting;
}

void clip_alarm_loop(void) {
    // Identify-blink (hub MSG_BLINK). Only drives the LED while the alarm is not
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
