#ifndef CLIP_ALARM_H
#define CLIP_ALARM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Pure alert-output driver (LED + passive buzzer). It does NOT know about the
// weekly schedule; the adherence module decides when to start/stop it.
// ---------------------------------------------------------------------------

// Configure the alert hardware. `led_pin` is driven active-high by default (see
// LED_ON/LED_OFF in clip_alarm.cpp); `buzzer_pin` is a passive buzzer driven with
// LEDC PWM (two-tone siren).
void clip_alarm_begin(uint8_t led_pin, uint8_t buzzer_pin);

// Start sounding the two-tone siren + blinking LED (idempotent).
void clip_alarm_start(void);

// Stop the siren and turn the LED off (idempotent).
void clip_alarm_stop(void);

// True while the alert is sounding.
bool clip_alarm_is_alerting(void);

// Housekeeping — call from loop(). Drives the siren/LED pattern non-blockingly,
// and runs the identify-blink (below) while the alert is quiet.
void clip_alarm_loop(void);

// Boot-time hardware self-test: blinks the LED three times and beeps the buzzer
// once. Blocking, so call it from setup() only.
void clip_alarm_self_test(void);

// Flash the LED for `seconds` seconds (hub's "identify" / MSG_BLINK). Non-
// blocking; driven from clip_alarm_loop().
void clip_alarm_blink(uint8_t seconds);

#ifdef __cplusplus
}
#endif

#endif // CLIP_ALARM_H
