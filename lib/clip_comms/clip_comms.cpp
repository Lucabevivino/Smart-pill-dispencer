#include "clip_comms.h"
#include "protocol.h"
#include "comms_ring.h"
#include "reliable_tx.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_attr.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

// --- tuning ---
#define CLIP_SCAN_START_CHANNEL 1
#define CLIP_SCAN_END_CHANNEL   13
#define CLIP_SCAN_DWELL_MS      600
#define CLIP_ACK_WAIT_MS        1000   // per-attempt wait for the hub's ACK
#define CLIP_ANNOUNCE_INTERVAL_MS 10000 // re-announce HELLO while unassigned

// Discovery/registration state, shared with the ESP-NOW receive callback (which
// runs in the ESP-NOW task). The callback only writes these; the main loop reads.
static uint8_t hub_mac[6];
static uint8_t channel = CLIP_SCAN_START_CHANNEL;
static volatile bool hub_found = false;
static volatile bool hub_ack = false;   // set when the hub acknowledges our HELLO
static uint8_t node_id = 0;             // our id, learned from the HELLO ACK
static bool ready = false;              // true once ESP-NOW + broadcast peer are up

// Assignment state.
static bool assigned = false;
static week_schedule schedule;
static volatile bool schedule_changed = false;   // set by ASSIGN / REMOVE handlers

// Inbound ring: the ESP-NOW callback buffers ASSIGN / BLINK / REMOVE /
// TIME_RESPONSE and adherence ACKs here; the main loop drains them.
static comms_ring_t inbound;

// Blink request drained by clip_comms_take_blink() (main loop drives the alarm).
static bool pending_blink = false;
static uint8_t pending_blink_seconds = 0;

// Per-type de-dup of hub->clip reliable messages. Each type has its own counter
// because the hub's seq is shared across types, so an interleaved retransmit of
// an earlier ASSIGN must still be recognized even after a later BLINK arrived.
// 0xFF (never a real seq) = "nothing seen yet" sentinel.
static uint8_t last_assign_seq = 0xFF;
static uint8_t last_blink_seq = 0xFF;
static uint8_t last_remove_seq = 0xFF;
static uint8_t last_time_seq = 0xFF;

// Re-announcement state. The hub releases us with MSG_REMOVE; while unassigned
// we keep re-sending HELLO so the hub never prunes us as "stale".
static uint32_t last_hello_ms = 0;

// Time re-sync state. last_resync_epoch survives deep sleep (persisted below);
// time_resynced is a per-wake flag set when a fresh time arrives this boot.
static time_t last_resync_epoch = 0;
static bool   time_resynced = false;

// Reliable outbound queue (adherence events). RTC slow memory so a pending
// "pill taken" survives deep sleep; on a cold power cycle RTC is zeroed and the
// queue starts empty (same model as the persist block below).
RTC_DATA_ATTR static reliable_tx_t tx = {0};

// Adherence events that could not be queued yet (hub unknown, or the reliable
// queue was full) are remembered here and flushed once we can. One bit per type
// is enough for a single clip: at most one TAKEN / MISSED / BOX_OPEN is pending
// between re-syncs. RTC-backed so a retained event survives deep sleep.
#define PENDING_TAKEN    (1u << 0)
#define PENDING_MISSED   (1u << 1)
#define PENDING_BOX_OPEN (1u << 2)
RTC_DATA_ATTR static uint8_t pending_events = 0;

// Defined later (adherence-event sends); forward-declared so clip_comms_loop
// can flush retained events.
static void flush_pending_events(void);

// Battery sensing. battery_pin == 0xFF disables reading (reports 0 mV). The
// ESP32-C3 ADC is ~3.3 V full-scale, so the divider ratio scales the raw reading
// back up to the actual battery voltage.
static uint8_t battery_pin = 0xFF;
static float   battery_divider = 1.0f;

// --- deep-sleep persistence (RTC slow memory) --------------------------------
// RTC_DATA_ATTR keeps this block in RTC slow memory, which survives deep sleep
// (but not a full power cycle / reset — a cold boot re-initializes it to zero,
// so magic == 0 means "nothing remembered").
#define CLIP_STATE_MAGIC 0x434C4950u   // "CLIP" — marks the block as ours

RTC_DATA_ATTR static struct {
    uint32_t magic;
    uint8_t  hub_mac[6];
    uint8_t  node_id;
    uint8_t  channel;
    bool     assigned;
    week_schedule schedule;
    int64_t  last_resync_epoch;   // wall-clock epoch of the last time sync
    uint8_t  crc;
} persist = {0};

// Weak 8-bit sum over the payload (hub_mac .. schedule). Good enough to catch
// typical bit rot in RTC memory; the magic is the real "was this written" guard.
static uint8_t state_checksum(const uint8_t* p, size_t n) {
    uint8_t sum = 0;
    while (n--) {
        sum += *p++;
    }
    return sum;
}

static void persist_save(void) {
    // Zero everything first so the padding bytes (covered by the checksum range)
    // are deterministic, not whatever RTC memory happened to hold.
    memset(&persist, 0, sizeof(persist));
    memcpy(persist.hub_mac, hub_mac, 6);
    persist.node_id  = node_id;
    persist.channel  = channel;
    persist.assigned = assigned;
    memcpy(&persist.schedule, &schedule, sizeof(schedule));
    persist.last_resync_epoch = last_resync_epoch;

    persist.magic = CLIP_STATE_MAGIC;
    persist.crc = state_checksum((const uint8_t*)persist.hub_mac,
                                 (const uint8_t*)&persist.crc - (const uint8_t*)persist.hub_mac);
}

static bool persist_restore(void) {
    if (persist.magic != CLIP_STATE_MAGIC) {
        return false;
    }
    uint8_t crc = state_checksum((const uint8_t*)persist.hub_mac,
                                 (const uint8_t*)&persist.crc - (const uint8_t*)persist.hub_mac);
    if (crc != persist.crc) {
        return false;   // corrupted: treat as "nothing remembered"
    }

    memcpy(hub_mac, persist.hub_mac, 6);
    node_id  = persist.node_id;
    channel  = persist.channel;
    assigned = persist.assigned;
    memcpy(&schedule, &persist.schedule, sizeof(schedule));
    last_resync_epoch = persist.last_resync_epoch;

    bool mac_set = false;
    for (int i = 0; i < 6; i++) {
        mac_set |= hub_mac[i] != 0;
    }
    if (mac_set) {
        hub_found = true;   // skip the scan on wake; remember the channel
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    }
    return true;
}

// --- helpers ---

static void print_mac(const uint8_t* mac) {
    Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool ensure_hub_peer(void) {
    if (esp_now_is_peer_exist(hub_mac)) {
        return true;   // already a peer: that's the desired end state
    }

    esp_now_peer_info_t hub;
    memset(&hub, 0, sizeof(hub));
    memcpy(hub.peer_addr, hub_mac, 6);
    hub.channel = channel;
    hub.ifidx = WIFI_IF_STA;
    hub.encrypt = false;
    if (esp_now_add_peer(&hub) != ESP_OK) {
        Serial.print("[clip] add_peer failed for central hub ");
        print_mac(hub_mac);
        Serial.println();
        return false;
    }
    return true;
}

// Set the system clock (the clip's "internal RTC") from a hub_time. Returns
// false if the time is invalid (e.g. the hub assigned before NTP synced) or the
// call failed. Wall-clock fields round-trip via localtime(), which is all the
// clip needs to compare against its schedule.
static bool set_rtc_from_hub_time(const hub_time* t) {
    if (t->year < 2000 || t->year > 2200 ||
        t->mon < 1 || t->mon > 12 ||
        t->mday < 1 || t->mday > 31 ||
        t->hour > 23 || t->min > 59 || t->sec > 59) {
        return false;   // the hub zero-fills hub_time when it assigns unsynced
    }

    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year  = (int)t->year - 1900;
    tm.tm_mon   = (int)t->mon - 1;
    tm.tm_mday  = (int)t->mday;
    tm.tm_hour  = (int)t->hour;
    tm.tm_min   = (int)t->min;
    tm.tm_sec   = (int)t->sec;
    tm.tm_isdst = 0;   // clip has no DST notion; keep it deterministic

    time_t epoch = mktime(&tm);
    if (epoch == (time_t)-1) {
        return false;
    }

    struct timeval tv;
    tv.tv_sec  = epoch;
    tv.tv_usec = 0;
    return settimeofday(&tv, NULL) == 0;
}

// Send an acknowledgement for a message the hub sent us. Called from the main
// loop (never the ESP-NOW callback), so Serial and esp_now_send are safe here.
static bool send_ack(uint8_t msg_to_ack, uint8_t seq) {
    if (!hub_found) {
        return false;
    }
    if (!ensure_hub_peer()) {
        return false;
    }

    ack_msg m;
    m.type       = MSG_ACK;
    m.seq        = seq;
    m.msg_to_ack = msg_to_ack;
    m.node_id    = node_id;

    if (esp_now_send(hub_mac, (const uint8_t*)&m, sizeof(m)) != ESP_OK) {
        return false;
    }
    Serial.print("[clip] ack sent (msg ");
    Serial.print((int)msg_to_ack);
    Serial.println(")");
    return true;
}

// Send a bare MSG_TIME_REQUEST (best-effort). The hub answers with a reliable
// MSG_TIME_RESPONSE, so a lost request just means the clip retries next wake.
static bool send_time_request(void) {
    if (!hub_found) {
        return false;
    }
    if (!ensure_hub_peer()) {
        return false;
    }
    time_request_msg m;
    m.type = MSG_TIME_REQUEST;
    return esp_now_send(hub_mac, (const uint8_t*)&m, sizeof(m)) == ESP_OK;
}

// Send function the reliable queue uses to transmit one frame to the hub.
static bool clip_reliable_send_fn(const uint8_t* dst, const uint8_t* data, uint8_t len) {
    if (!ensure_hub_peer()) {
        return false;
    }
    return esp_now_send(dst, data, len) == ESP_OK;
}

// De-dup helper: true if `seq` was already processed (a retransmission).
static bool is_duplicate(uint8_t* last, uint8_t seq) {
    if (seq == *last) {
        return true;
    }
    *last = seq;
    return false;
}

// --- reliable-message handlers (main loop) ---

static void handle_assign(const assign_msg* a) {
    if (is_duplicate(&last_assign_seq, a->seq)) {
        send_ack(MSG_ASSIGN, a->seq);   // re-ACK, don't reprocess
        return;
    }

    node_id = a->node_id;
    memcpy(&schedule, &a->schedule, sizeof(schedule));
    assigned = true;
    schedule_changed = true;

    Serial.print("[clip] assigned node #");
    Serial.println((int)node_id);

    if (set_rtc_from_hub_time(&a->time)) {
        Serial.printf("[clip] RTC set to %d-%02d-%02d %02d:%02d:%02d\n",
                      (int)a->time.year, (int)a->time.mon, (int)a->time.mday,
                      (int)a->time.hour, (int)a->time.min, (int)a->time.sec);
        last_resync_epoch = time(NULL);
    } else {
        Serial.println("[clip] RTC not set (no valid time in assign)");
    }

    send_ack(MSG_ASSIGN, a->seq);
    persist_save();

    // Report battery on the fresh pairing so the hub has a reading immediately,
    // rather than waiting for the first hourly re-sync.
    clip_comms_send_battery();
}

static void handle_blink(const blink_msg* b) {
    if (is_duplicate(&last_blink_seq, b->seq)) {
        send_ack(MSG_BLINK, b->seq);
        return;
    }
    pending_blink_seconds = b->seconds;
    pending_blink = true;
    send_ack(MSG_BLINK, b->seq);
}

static void handle_remove(const remove_msg* r) {
    if (is_duplicate(&last_remove_seq, r->seq)) {
        send_ack(MSG_REMOVE, r->seq);
        return;
    }
    assigned = false;
    memset(&schedule, 0, sizeof(schedule));
    schedule_changed = true;
    last_hello_ms = 0;   // announce again immediately
    Serial.println("[clip] removed by hub - free again");
    send_ack(MSG_REMOVE, r->seq);
    persist_save();
}

static void handle_time_response(const time_response_msg* t) {
    if (is_duplicate(&last_time_seq, t->seq)) {
        send_ack(MSG_TIME_RESPONSE, t->seq);
        return;
    }
    if (set_rtc_from_hub_time(&t->time)) {
        Serial.printf("[clip] RTC re-synced to %d-%02d-%02d %02d:%02d:%02d\n",
                      (int)t->time.year, (int)t->time.mon, (int)t->time.mday,
                      (int)t->time.hour, (int)t->time.min, (int)t->time.sec);
        last_resync_epoch = time(NULL);
        time_resynced = true;
        persist_save();
    }
    send_ack(MSG_TIME_RESPONSE, t->seq);
}

static void handle_adherence_ack(const ack_msg* a) {
    reliable_on_ack(&tx, a->seq);
    Serial.print("[clip] event acked (seq ");
    Serial.print((int)a->seq);
    Serial.println(")");
}

// --- ESP-NOW callbacks (ESP-NOW task; keep them short, no Serial / delays) ---

static void on_data_recv(const uint8_t* mac, const uint8_t* data, int len) {
    if (len < 1) {
        return;
    }
    switch (data[0]) {
    case MSG_BEACON:
        if (len >= (int)sizeof(beacon_msg) &&
            ((const beacon_msg*)data)->version == PROTOCOL_VERSION) {
            memcpy(hub_mac, mac, 6);
            hub_found = true;   // set after the MAC is written
        }
        break;
    case MSG_ACK:
        if (len >= (int)sizeof(ack_msg)) {
            const ack_msg* a = (const ack_msg*)data;
            if (a->msg_to_ack == MSG_HELLO) {
                // Registration handshake: single-slot write + flag, like before.
                node_id = a->node_id;
                hub_ack = true;   // set after validating
            } else {
                // Adherence ACK: buffer it; the loop frees the matching frame.
                comms_ring_push(&inbound, mac, data, (uint8_t)len);
            }
        }
        break;
    case MSG_ASSIGN:
    case MSG_BLINK:
    case MSG_REMOVE:
    case MSG_TIME_RESPONSE:
        comms_ring_push(&inbound, mac, data, (uint8_t)len);
        break;
    default:
        break;
    }
}

static void on_data_sent(const uint8_t* mac, esp_now_send_status_t status) {
    reliable_on_sent(&tx, mac, status == ESP_NOW_SEND_SUCCESS);
    if (status != ESP_NOW_SEND_SUCCESS) {
        Serial.print("[clip] send FAIL to ");
        print_mac(mac);
        Serial.println();
    }
}

// --- public API ---

void clip_comms_begin(void) {
    WiFi.mode(WIFI_STA);

    if (esp_now_init() != ESP_OK) {
        Serial.println("[clip] esp_now_init failed");
        return;
    }
    esp_now_register_recv_cb(on_data_recv);
    esp_now_register_send_cb(on_data_sent);

    reliable_init(&tx, clip_reliable_send_fn);

    // Register the broadcast address so beacons are delivered while we hop
    // channels. channel 0 = "current channel", so it follows the hops.
    esp_now_peer_info_t bcast;
    memset(&bcast, 0, sizeof(bcast));
    memcpy(bcast.peer_addr, ESPNOW_BROADCAST_MAC, 6);
    bcast.channel = 0;
    bcast.ifidx = WIFI_IF_STA;
    bcast.encrypt = false;
    if (esp_now_add_peer(&bcast) != ESP_OK) {
        Serial.println("[clip] add broadcast peer failed");
        return;
    }

    ready = true;

    // Restore a hub remembered from before deep sleep, so the clip can skip the
    // channel scan on wake-up.
    if (persist_restore()) {
        Serial.print("[clip] restored hub ");
        print_mac(hub_mac);
        Serial.print(" on channel ");
        Serial.println((int)channel);
    }

    Serial.println("[clip] ESP-NOW ready");
}

bool clip_comms_find_hub(uint32_t timeout_ms) {
    if (!ready) {
        return false;
    }

    uint32_t start = millis();

    while (!hub_found && millis() - start < timeout_ms) {
        for (uint8_t ch = CLIP_SCAN_START_CHANNEL;
             ch <= CLIP_SCAN_END_CHANNEL && !hub_found;
             ch++) {
            channel = ch;
            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);

            uint32_t dwell_start = millis();
            while (!hub_found && millis() - dwell_start < CLIP_SCAN_DWELL_MS) {
                delay(10);
            }
        }
    }

    if (hub_found) {
        Serial.print("[clip] found hub ");
        print_mac(hub_mac);
        Serial.print(" on channel ");
        Serial.println((int)channel);
        persist_save();
        return true;
    }

    Serial.println("[clip] hub not found");
    return false;
}

bool clip_comms_send_hello(void) {
    if (!hub_found) {
        return false;
    }
    last_hello_ms = millis();   // all HELLO sends share one throttle timer
    if (!ensure_hub_peer()) {
        Serial.println("[clip] ensure_hub_peer failed");
        return false;
    }

    hello_msg m;
    m.type = MSG_HELLO;
    m.version = PROTOCOL_VERSION;

    if (esp_now_send(hub_mac, (const uint8_t*)&m, sizeof(m)) != ESP_OK) {
        return false;
    }
    Serial.print("[clip] hello sent to ");
    print_mac(hub_mac);
    Serial.println();
    return true;
}

bool clip_comms_register(uint32_t timeout_ms) {
    if (!hub_found) {
        return false;
    }

    uint32_t start = millis();
    while (millis() - start < timeout_ms) {
        hub_ack = false;

        if (clip_comms_send_hello()) {
            uint32_t ack_start = millis();
            while (!hub_ack && millis() - ack_start < CLIP_ACK_WAIT_MS) {
                delay(10);
            }
            if (hub_ack) {
                Serial.print("[clip] registered with hub as node #");
                Serial.println((int)node_id);
                persist_save();
                return true;
            }
        }

        delay(50);   // small gap before the next attempt
    }

    Serial.println("[clip] hub did not acknowledge hello");
    return false;
}

void clip_comms_loop(void) {
    // Drain buffered hub->clip frames. All handlers run on the main loop, so
    // Serial / esp_now_send / reliable_tx access are safe here.
    uint8_t mac[6];
    uint8_t buf[COMMS_RING_MAX_PAYLOAD];
    uint8_t len;
    while (comms_ring_pop(&inbound, mac, buf, &len)) {
        if (len < 1) {
            continue;
        }
        switch (buf[0]) {
        case MSG_ASSIGN:
            if (len >= sizeof(assign_msg)) {
                handle_assign((const assign_msg*)buf);
            }
            break;
        case MSG_BLINK:
            if (len >= sizeof(blink_msg)) {
                handle_blink((const blink_msg*)buf);
            }
            break;
        case MSG_REMOVE:
            if (len >= sizeof(remove_msg)) {
                handle_remove((const remove_msg*)buf);
            }
            break;
        case MSG_TIME_RESPONSE:
            if (len >= sizeof(time_response_msg)) {
                handle_time_response((const time_response_msg*)buf);
            }
            break;
        case MSG_ACK:
            if (len >= sizeof(ack_msg)) {
                handle_adherence_ack((const ack_msg*)buf);
            }
            break;
        default:
            break;
        }
    }

    // Retransmit any adherence events still awaiting the hub's ACK.
    reliable_loop(&tx);

    // Flush any adherence events retained while the hub was unknown / queue full.
    flush_pending_events();

    // While known to the hub but unassigned, keep re-announcing so the hub
    // doesn't prune our slot as stale (it forgets unassigned nodes it hasn't
    // heard from in HUB_STALE_NODE_MS).
    if (hub_found && !assigned &&
        millis() - last_hello_ms >= CLIP_ANNOUNCE_INTERVAL_MS) {
        clip_comms_send_hello();   // updates last_hello_ms
    }
}

bool clip_comms_is_connected(void) {
    return hub_found;
}

void clip_comms_forget_hub(void) {
    if (hub_found) {
        // Drop the stale peer so a re-discovered hub (new channel / MAC) isn't
        // blocked by an old peer entry still pinned to the previous channel.
        esp_now_del_peer(hub_mac);
    }
    hub_found = false;
    memset(hub_mac, 0, sizeof(hub_mac));
}

bool clip_comms_is_registered(void) {
    return hub_ack;
}

uint8_t clip_comms_node_id(void) {
    return node_id;
}

bool clip_comms_is_assigned(void) {
    return assigned;
}

const week_schedule* clip_comms_schedule(void) {
    return assigned ? &schedule : NULL;
}

bool clip_comms_take_schedule_changed(void) {
    if (!schedule_changed) {
        return false;
    }
    schedule_changed = false;
    return true;
}

bool clip_comms_take_blink(uint8_t* seconds) {
    if (!pending_blink) {
        return false;
    }
    pending_blink = false;
    if (seconds != NULL) {
        *seconds = pending_blink_seconds;
    }
    return true;
}

void clip_comms_hub_mac(uint8_t out[6]) {
    memcpy(out, hub_mac, 6);
}

uint8_t clip_comms_channel(void) {
    return channel;
}

// --- time re-sync ---

bool clip_comms_request_time_sync(uint32_t timeout_ms) {
    if (!hub_found) {
        return false;
    }
    time_resynced = false;
    uint32_t start = millis();
    uint32_t last_req = 0;
    while (millis() - start < timeout_ms) {
        if (millis() - last_req >= 500) {
            last_req = millis();
            send_time_request();
        }
        clip_comms_loop();   // drains MSG_TIME_RESPONSE -> handle_time_response
        if (time_resynced) {
            return true;
        }
        delay(10);
    }
    return false;
}

time_t clip_comms_last_resync_epoch(void) {
    return last_resync_epoch;
}

bool clip_comms_resync_due(time_t now_epoch) {
    if (last_resync_epoch == 0) {
        return true;   // never synced — re-sync at the first opportunity
    }
    return (now_epoch - last_resync_epoch) >= (time_t)CLIP_RESYNC_INTERVAL_SEC;
}

// --- battery reporting ---

void clip_comms_set_battery(uint8_t pin, float divider) {
    battery_pin = pin;
    battery_divider = divider;
    if (battery_pin != 0xFF) {
        analogReadResolution(12);      // 0..4095
        pinMode(battery_pin, INPUT);
    }
}

static uint16_t read_battery_mv(void) {
    if (battery_pin == 0xFF) {
        return 0;
    }
    int raw = analogRead(battery_pin);
    float mv = (float)raw * 3300.0f / 4095.0f * battery_divider;
    if (mv < 0.0f) {
        mv = 0.0f;
    } else if (mv > 65535.0f) {
        mv = 65535.0f;
    }
    return (uint16_t)mv;
}

bool clip_comms_send_battery(void) {
    if (!hub_found) {
        return false;
    }
    battery_msg m;
    m.type = MSG_BATTERY;
    m.seq = 0;          // stamped by reliable_send
    m.node_id = node_id;
    m.mv = read_battery_mv();

    Serial.printf("[clip] battery %u mV\n", (unsigned)m.mv);
    return reliable_send(&tx, hub_mac, &m, sizeof(m));
}

// --- adherence event sends ---

// Queue a minimal (type, seq, node_id) adherence message for reliable delivery.
// Returns false only if the hub is unknown or the queue is full; otherwise the
// event is retained and retried until the hub acknowledges it.
// Queue a minimal (type, seq, node_id) adherence message for reliable delivery.
// Returns false only if the hub is unknown or the queue is full; in that case
// the event is retained (pending_events) and flushed by clip_comms_loop().
static bool send_node_event(uint8_t type) {
    // taken_msg / missed_msg / box_open_msg share the same packed 3-byte layout
    // (type, seq, node_id); use the shared typedef so the clip and hub parser
    // can never drift apart.
    taken_msg m;
    m.type = type;
    m.seq = 0;         // stamped by reliable_send
    m.node_id = node_id;

    uint8_t bit;
    switch (type) {
    case MSG_TAKEN:    bit = PENDING_TAKEN;    break;
    case MSG_MISSED:   bit = PENDING_MISSED;   break;
    case MSG_BOX_OPEN: bit = PENDING_BOX_OPEN; break;
    default:           bit = 0;                break;
    }

    if (!hub_found || !reliable_send(&tx, hub_mac, &m, sizeof(m))) {
        if (bit != 0) {
            pending_events |= bit;   // retain for a later flush
        }
        return false;
    }
    return true;
}

// Send any adherence events that were retained because the hub was unknown or
// the queue was full. Called from the main loop once discovery/registration is
// healthy again.
static void flush_pending_events(void) {
    if (!hub_found || pending_events == 0) {
        return;
    }
    static const struct { uint8_t bit; uint8_t type; } pend[] = {
        {PENDING_TAKEN,    MSG_TAKEN},
        {PENDING_MISSED,   MSG_MISSED},
        {PENDING_BOX_OPEN, MSG_BOX_OPEN},
    };
    for (unsigned i = 0; i < sizeof(pend) / sizeof(pend[0]); i++) {
        if (pending_events & pend[i].bit) {
            taken_msg m;
            m.type = pend[i].type;
            m.seq = 0;
            m.node_id = node_id;
            if (reliable_send(&tx, hub_mac, &m, sizeof(m))) {
                pending_events &= (uint8_t)~pend[i].bit;
            }
        }
    }
}

bool clip_comms_send_taken(void) {
    return send_node_event(MSG_TAKEN);
}

bool clip_comms_send_missed(void) {
    return send_node_event(MSG_MISSED);
}

bool clip_comms_send_box_open(void) {
    return send_node_event(MSG_BOX_OPEN);
}
