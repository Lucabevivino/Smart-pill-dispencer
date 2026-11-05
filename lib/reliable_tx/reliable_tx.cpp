#include "reliable_tx.h"
#include <string.h>
#include <Arduino.h>     // millis()
#include <esp_random.h>  // esp_random()

#define RELIABLE_TX_MAGIC 0x52545821u   // "RTX!" — marks the queue as ours

// Backoff for the next retry after `attempts` transmissions already happened:
// base, 2*base, 4*base, … capped at max_retry_ms.
static uint32_t backoff_ms(const reliable_tx_t* t, uint8_t attempts) {
    uint32_t d = t->base_retry_ms;
    for (uint8_t i = 1; i < attempts && d < t->max_retry_ms; i++) {
        d *= 2;
    }
    return (d > t->max_retry_ms) ? t->max_retry_ms : d;
}

static void reset_volatile(reliable_tx_t* t) {
    // Do not touch frames/next_seq/magic: on a deep-sleep wake they are the
    // pending queue and must survive. Only reset per-boot state.
    t->base_retry_ms = 1000;
    t->max_retry_ms = 30000;
}

void reliable_init(reliable_tx_t* t,
                   bool (*send_fn)(const uint8_t* dst, const uint8_t* data, uint8_t len)) {
    t->send_fn = send_fn;
    reset_volatile(t);

    if (t->magic != RELIABLE_TX_MAGIC) {
        // Cold boot (RTC memory zeroed, or a hub in plain RAM): start empty.
        memset(t->frames, 0, sizeof(t->frames));
        // Random starting seq so a sender reboot is very unlikely to re-issue a
        // seq a peer still holds in its de-dup map (which would false-dedup the
        // first message after the reboot). 1..255, never 0.
        t->next_seq = (uint8_t)(1 + (esp_random() & 0xFE));
        t->magic = RELIABLE_TX_MAGIC;
        return;
    }

    // Deep-sleep wake with a valid queue: keep pending frames, but re-arm their
    // retry timers so they go out again as soon as the loop runs.
    for (int i = 0; i < RELIABLE_QUEUE_SIZE; i++) {
        if (t->frames[i].used) {
            t->frames[i].next_retry_ms = 0;
            t->frames[i].attempts = 0;
        }
    }
}

bool reliable_send(reliable_tx_t* t, const uint8_t* dst, const void* data, uint8_t len) {
    if (len > RELIABLE_MAX_PAYLOAD) {
        return false;
    }
    reliable_frame_t* f = NULL;
    for (int i = 0; i < RELIABLE_QUEUE_SIZE; i++) {
        if (!t->frames[i].used) {
            f = &t->frames[i];
            break;
        }
    }
    if (f == NULL) {
        return false;   // queue full — drop the newest
    }

    memcpy(f->dst, dst, 6);
    memcpy(f->data, data, len);
    f->len = len;
    f->seq = t->next_seq++;
    if (len >= 2) {
        f->data[1] = f->seq;   // seq is the 2nd byte of every reliable message
    }
    f->attempts = 0;
    f->used = true;

    // Transmit once right away, then schedule the first retry if unacked.
    if (t->send_fn != NULL) {
        t->send_fn(f->dst, f->data, f->len);
        f->attempts = 1;
        f->next_retry_ms = millis() + backoff_ms(t, f->attempts);
    }
    return true;
}

void reliable_loop(reliable_tx_t* t) {
    if (t->send_fn == NULL) {
        return;
    }
    uint32_t now = millis();
    for (int i = 0; i < RELIABLE_QUEUE_SIZE; i++) {
        reliable_frame_t* f = &t->frames[i];
        if (!f->used || f->next_retry_ms > now) {
            continue;
        }
        t->send_fn(f->dst, f->data, f->len);
        f->attempts++;
        f->next_retry_ms = now + backoff_ms(t, f->attempts);
    }
}

void reliable_on_sent(reliable_tx_t* t, const uint8_t* mac, bool ok) {
    if (ok) {
        return;   // link-layer success — the app ACK (timeout) is the real proof
    }
    for (int i = 0; i < RELIABLE_QUEUE_SIZE; i++) {
        reliable_frame_t* f = &t->frames[i];
        if (f->used && memcmp(f->dst, mac, 6) == 0) {
            f->next_retry_ms = 0;   // retry immediately
        }
    }
}

void reliable_on_ack(reliable_tx_t* t, uint8_t seq) {
    for (int i = 0; i < RELIABLE_QUEUE_SIZE; i++) {
        reliable_frame_t* f = &t->frames[i];
        if (f->used && f->seq == seq) {
            f->used = false;   // delivered — free the slot
            return;
        }
    }
}
