#include "hub_comms.h"
#include "node_management.h"
#include "comms_ring.h"
#include "reliable_tx.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <time.h>
#include <string.h>
#include "nvs.h"
#include "esp_err.h"

// --- tuning ---
#define HUB_TICK_INTERVAL_MS       100     // min gap between housekeeping runs
#define HUB_BEACON_INTERVAL_MS     250     // poll for new nodes; keep < the clip's
                                           // CLIP_SCAN_DWELL_MS so one dwell reliably
                                           // catches a beacon while the clip hops channels
#define HUB_STALE_NODE_MS          30000   // unassigned node considered gone
#define HUB_WIFI_RETRY_INTERVAL_MS 10000
#define HUB_ESPNOW_FALLBACK_CHANNEL 1

static const char* NTP_SERVERS[] = {"pool.ntp.org", "time.nist.gov", "time.google.com"};

static uint8_t channel = HUB_ESPNOW_FALLBACK_CHANNEL;

static uint32_t last_tick_ms = 0;
static uint32_t last_beacon_ms = 0;
static uint32_t last_wifi_retry_ms = 0;

// Inbound SPSC rings: the ESP-NOW callback pushes, the main loop pops. Ring
// (not single-slot) so simultaneous HELLOs / time requests / ACKs aren't lost.
static comms_ring_t hello_ring;
static comms_ring_t time_ring;
static comms_ring_t ack_ring;

// Adherence events (MSG_TAKEN / MSG_MISSED / MSG_BOX_OPEN) and battery reports
// (MSG_BATTERY) from clips. A small ring buffer (single producer: the ESP-NOW
// callback; single consumer: the main loop) with the sender's seq, so
// retransmissions can be de-duplicated and ACKed.
#define HUB_EVENT_QUEUE_SIZE 8
static volatile uint8_t  event_types[HUB_EVENT_QUEUE_SIZE];
static volatile uint8_t  event_node_ids[HUB_EVENT_QUEUE_SIZE];
static volatile uint8_t  event_seqs[HUB_EVENT_QUEUE_SIZE];
static volatile uint16_t event_mvs[HUB_EVENT_QUEUE_SIZE];   // MSG_BATTERY only
static volatile uint8_t  event_head = 0;   // next write slot (callback)
static volatile uint8_t  event_tail = 0;   // next read slot (main loop)

// Most recent battery voltage reported per node (millivolts), 0 if none yet.
static uint16_t last_battery_mv[MAX_NODES + 1];

// --- persistent adherence log ------------------------------------------------
// Every fresh adherence event (MSG_TAKEN / MSG_MISSED / MSG_BOX_OPEN) is appended
// to a bounded ring log that survives reboot. This does two jobs at once: (1) it
// is tomorrow's server client's source of truth for what to forward to the DB,
// and (2) it persists the per-node de-dup watermark (last_seq), so a hub reboot
// can't re-apply a retained event and double-count it.
#define HUB_ADHERENCE_LOG_MAX 128
typedef struct {
    uint8_t  node_id;
    uint8_t  type;
    uint8_t  seq;
    uint32_t timestamp;   // epoch seconds when the event was applied
} adherence_log_entry_t;

typedef struct {
    uint32_t magic;
    uint16_t count;                    // entries currently stored (0..MAX)
    uint16_t start;                    // ring index of the oldest entry
    uint8_t  last_seq[MAX_NODES + 1];  // de-dup watermark, 0xFF = "none yet"
    adherence_log_entry_t entries[HUB_ADHERENCE_LOG_MAX];
} hub_adherence_persist_t;

#define HUB_ADHERENCE_LOG_MAGIC 0x41444C47u   // "ADLG"
#define HUB_LOG_NVS_NAMESPACE   "adhlog"
#define HUB_LOG_NVS_KEY         "log"

// The live log + de-dup state. `adhlog.last_seq` is the authoritative de-dup
// watermark (replaces the old RAM-only last_adherence_seq[]).
static hub_adherence_persist_t adhlog;

static void adherence_log_save(void) {
    nvs_handle_t h;
    if (nvs_open(HUB_LOG_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    adhlog.magic = HUB_ADHERENCE_LOG_MAGIC;
    nvs_set_blob(h, HUB_LOG_NVS_KEY, &adhlog, sizeof(adhlog));
    nvs_commit(h);
    nvs_close(h);
}

static void adherence_log_restore(void) {
    memset(&adhlog, 0, sizeof(adhlog));
    for (int i = 0; i <= MAX_NODES; i++) {
        adhlog.last_seq[i] = 0xFF;   // sentinel: "no event seen yet"
    }

    nvs_handle_t h;
    if (nvs_open(HUB_LOG_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;   // nothing persisted yet — keep the empty log
    }
    size_t len = sizeof(adhlog);
    if (nvs_get_blob(h, HUB_LOG_NVS_KEY, &adhlog, &len) == ESP_OK &&
        len == sizeof(adhlog) && adhlog.magic == HUB_ADHERENCE_LOG_MAGIC) {
        // Clamp in case HUB_ADHERENCE_LOG_MAX shrank across a firmware change.
        if (adhlog.count > HUB_ADHERENCE_LOG_MAX) {
            adhlog.count = HUB_ADHERENCE_LOG_MAX;
        }
        if (adhlog.start >= HUB_ADHERENCE_LOG_MAX) {
            adhlog.start = 0;
        }
    } else {
        // Corrupt or missing: reset to the empty-log defaults.
        memset(&adhlog, 0, sizeof(adhlog));
        for (int i = 0; i <= MAX_NODES; i++) {
            adhlog.last_seq[i] = 0xFF;
        }
    }
    nvs_close(h);
}

static void adherence_log_append(uint8_t node_id, uint8_t type, uint8_t seq,
                                 uint32_t timestamp) {
    adherence_log_entry_t e;
    e.node_id = node_id;
    e.type = type;
    e.seq = seq;
    e.timestamp = timestamp;

    if (adhlog.count < HUB_ADHERENCE_LOG_MAX) {
        adhlog.entries[(adhlog.start + adhlog.count) % HUB_ADHERENCE_LOG_MAX] = e;
        adhlog.count++;
    } else {
        adhlog.entries[adhlog.start] = e;   // full: overwrite the oldest entry
        adhlog.start = (uint16_t)((adhlog.start + 1) % HUB_ADHERENCE_LOG_MAX);
    }
    adherence_log_save();
}

// Reliable outbound queue (assign / blink / remove / time response). Persisted
// to NVS so an un-ACKed message survives a hub reboot (the clip de-dups by seq,
// so a re-sent message after reboot is harmless).
static reliable_tx_t tx;

// Optional schedule source (registered by main.cpp) used to re-ASSIGN a rebooted
// clip that re-HELLOs. Returns the week_schedule for an assigned node, or NULL.
static const week_schedule* (*schedule_source)(int node_id) = NULL;

#define HUB_TX_NVS_NAMESPACE "txq"
#define HUB_TX_NVS_KEY       "queue"

// Persistent portion of the queue. send_fn / base / max / attempts / retry
// deadlines are volatile and re-established by reliable_init on restore.
typedef struct {
    uint32_t         magic;
    uint8_t          next_seq;
    reliable_frame_t frames[RELIABLE_QUEUE_SIZE];
} hub_tx_persist_t;

static void hub_tx_save(const reliable_tx_t* t) {
    nvs_handle_t h;
    if (nvs_open(HUB_TX_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    hub_tx_persist_t p;
    memset(&p, 0, sizeof(p));
    p.magic    = RELIABLE_TX_MAGIC;
    p.next_seq = t->next_seq;
    memcpy(p.frames, t->frames, sizeof(p.frames));
    nvs_set_blob(h, HUB_TX_NVS_KEY, &p, sizeof(p));
    nvs_commit(h);
    nvs_close(h);
}

static void hub_tx_restore(reliable_tx_t* t) {
    nvs_handle_t h;
    if (nvs_open(HUB_TX_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;   // nothing persisted yet — reliable_init will cold-boot the queue
    }
    hub_tx_persist_t p;
    size_t len = sizeof(p);
    if (nvs_get_blob(h, HUB_TX_NVS_KEY, &p, &len) == ESP_OK && len == sizeof(p) &&
        p.magic == RELIABLE_TX_MAGIC) {
        t->next_seq = (p.next_seq == 0) ? 1 : p.next_seq;   // 0 is the reserved sentinel
        memcpy(t->frames, p.frames, sizeof(t->frames));
        t->magic = RELIABLE_TX_MAGIC;   // reliable_init will keep the frames
        reliable_seal(t);               // recompute the checksum over the restored queue
    }
    nvs_close(h);
}

// --- helpers ---

static bool fetch_time(hub_time* out) {
    struct tm t;
    if (!getLocalTime(&t, 0)) {
        return false;
    }
    out->year    = (uint16_t)(t.tm_year + 1900);
    out->mon     = (uint8_t)(t.tm_mon + 1);
    out->mday    = (uint8_t)t.tm_mday;
    out->hour    = (uint8_t)t.tm_hour;
    out->min     = (uint8_t)t.tm_min;
    out->sec     = (uint8_t)t.tm_sec;
    out->weekday = (uint8_t)((t.tm_wday + 6) % 7);   // 0 = Monday ... 6 = Sunday
    return true;
}

static void print_mac(const uint8_t* mac) {
    Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool is_broadcast(const uint8_t* mac) {
    return memcmp(mac, ESPNOW_BROADCAST_MAC, 6) == 0;
}

static void ensure_peer(const uint8_t* mac) {
    if (esp_now_is_peer_exist(mac)) {
        return;
    }
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) {
        Serial.print("[hub] add_peer failed for ");
        print_mac(mac);
        Serial.println();
    }
}

// Send function the reliable queue uses to transmit one frame.
static bool reliable_send_fn(const uint8_t* dst, const uint8_t* data, uint8_t len) {
    ensure_peer(dst);
    return esp_now_send(dst, data, len) == ESP_OK;
}

// Best-effort ACK for an adherence event. Sent from the main loop; if it is
// lost the clip retransmits the event and we ACK again (no ACK-of-ACK).
static void hub_ack_event(uint8_t node_id, uint8_t msg_type, uint8_t seq) {
    uint8_t mac[6];
    if (!node_get_mac(node_id, mac)) {
        return;
    }
    ensure_peer(mac);
    ack_msg a;
    a.type = MSG_ACK;
    a.seq = seq;
    a.msg_to_ack = msg_type;
    a.node_id = node_id;
    esp_now_send(mac, (const uint8_t*)&a, sizeof(a));
}

// Called from the ESP-NOW receive callback only.
static void queue_event(uint8_t type, uint8_t node_id, uint8_t seq, uint16_t mv) {
    uint8_t next = (uint8_t)((event_head + 1) % HUB_EVENT_QUEUE_SIZE);
    if (next == event_tail) {
        return;   // queue full — drop the newest event rather than clobber one
    }
    event_types[event_head]    = type;
    event_node_ids[event_head] = node_id;
    event_seqs[event_head]     = seq;
    event_mvs[event_head]      = mv;
    event_head = next;
}

// --- ESP-NOW callbacks (ESP-NOW task; keep them short, no LCD / delays / sends) ---

static void on_data_recv(const uint8_t* mac, const uint8_t* data, int len) {
    if (len < 1) {
        return;
    }
    switch (data[0]) {
    case MSG_HELLO:
        if (len >= (int)sizeof(hello_msg) &&
            ((const hello_msg*)data)->version == PROTOCOL_VERSION) {
            comms_ring_push(&hello_ring, mac, data, (uint8_t)len);
        }
        break;
    case MSG_TIME_REQUEST:
        comms_ring_push(&time_ring, mac, data, (uint8_t)len);
        break;
    case MSG_ACK:
        comms_ring_push(&ack_ring, mac, data, (uint8_t)len);
        break;
    case MSG_TAKEN:
        if (len >= (int)sizeof(taken_msg)) {
            queue_event(MSG_TAKEN, ((const taken_msg*)data)->node_id,
                        ((const taken_msg*)data)->seq, 0);
        }
        break;
    case MSG_MISSED:
        if (len >= (int)sizeof(missed_msg)) {
            queue_event(MSG_MISSED, ((const missed_msg*)data)->node_id,
                        ((const missed_msg*)data)->seq, 0);
        }
        break;
    case MSG_BOX_OPEN:
        if (len >= (int)sizeof(box_open_msg)) {
            queue_event(MSG_BOX_OPEN, ((const box_open_msg*)data)->node_id,
                        ((const box_open_msg*)data)->seq, 0);
        }
        break;
    case MSG_BATTERY:
        if (len >= (int)sizeof(battery_msg)) {
            queue_event(MSG_BATTERY, ((const battery_msg*)data)->node_id,
                        ((const battery_msg*)data)->seq, ((const battery_msg*)data)->mv);
        }
        break;
    default:
        break;   // the hub doesn't receive the hub->node message types
    }
}

static void on_data_sent(const uint8_t* mac, esp_now_send_status_t status) {
    reliable_on_sent(&tx, mac, status == ESP_NOW_SEND_SUCCESS);
    if (is_broadcast(mac)) {
        return;   // don't spam the log with beacons
    }
    Serial.print("[hub] send to ");
    print_mac(mac);
    Serial.println(status == ESP_NOW_SEND_SUCCESS ? " OK" : " FAIL");
}

// --- public API ---

bool hub_comms_begin(const char* ssid, const char* password,
                     int gmt_off_sec, int daylight_off_sec,
                     uint32_t wifi_timeout_ms, uint32_t ntp_timeout_ms) {
    // 1) WiFi STA (needed for NTP; ESP-NOW rides its channel).
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < wifi_timeout_ms) {
        delay(250);
    }

    // 2) NTP. SNTP keeps syncing in the background after this; the wait below is
    //    just a head start so assignment has a valid clock as early as possible.
    configTime(gmt_off_sec, daylight_off_sec,
               NTP_SERVERS[0], NTP_SERVERS[1], NTP_SERVERS[2]);
    bool time_synced = false;
    start = millis();
    while (!time_synced && millis() - start < ntp_timeout_ms) {
        hub_time t;
        time_synced = fetch_time(&t);
        if (!time_synced) {
            delay(250);
        }
    }

    // 3) ESP-NOW on the current WiFi channel (fallback if WiFi didn't connect).
    channel = (WiFi.status() == WL_CONNECTED)
                  ? WiFi.channel()
                  : (uint8_t)HUB_ESPNOW_FALLBACK_CHANNEL;

    if (esp_now_init() != ESP_OK) {
        Serial.println("[hub] esp_now_init failed");
        return false;
    }
    esp_now_register_recv_cb(on_data_recv);
    esp_now_register_send_cb(on_data_sent);

    hub_tx_restore(&tx);                 // load any queue persisted before a reboot
    reliable_init(&tx, reliable_send_fn);
    tx.persist_fn = hub_tx_save;         // persist the queue on every change

    // Restore the adherence log + de-dup watermark from NVS. 0xFF = "no event
    // seen yet" (seq 0 is reserved, so no real event can alias it).
    adherence_log_restore();
    memset(last_battery_mv, 0, sizeof(last_battery_mv));

    esp_now_peer_info_t bcast;
    memset(&bcast, 0, sizeof(bcast));
    memcpy(bcast.peer_addr, ESPNOW_BROADCAST_MAC, 6);
    bcast.channel = channel;
    bcast.ifidx = WIFI_IF_STA;
    bcast.encrypt = false;
    if (esp_now_add_peer(&bcast) != ESP_OK) {
        Serial.println("[hub] add broadcast peer failed");
        return false;
    }

    Serial.printf("[hub] ESP-NOW ready on channel %d%s\n",
                  (int)channel, time_synced ? ", time synced" : ", time pending");
    return true;
}

void hub_comms_loop(void) {
    uint32_t now = millis();
    if (now - last_tick_ms < HUB_TICK_INTERVAL_MS) {
        return;   // rate-limit the housekeeping
    }
    last_tick_ms = now;

    // Retransmit any queued assign/blink/remove/time_response awaiting an ACK.
    reliable_loop(&tx);

    // Drain pending HELLOs (a burst registers every clip that announced).
    uint8_t mac[6];
    while (comms_ring_pop(&hello_ring, mac, NULL, NULL)) {
        int id = node_register(mac, now);
        if (id) {
            Serial.print("[hub] node #");
            Serial.print(id);
            Serial.print(" (");
            print_mac(mac);
            Serial.println(") wants to connect");

            ensure_peer(mac);
            ack_msg ack_m;
            ack_m.type = MSG_ACK;
            ack_m.seq = 0;                 // HELLO handshake is not sequenced
            ack_m.msg_to_ack = MSG_HELLO;
            ack_m.node_id = (uint8_t)id;
            if (esp_now_send(mac, (const uint8_t*)&ack_m, sizeof(ack_m)) == ESP_OK) {
                Serial.print("[hub] ack sent to ");
                print_mac(mac);
                Serial.println();
            }

            // Re-ASSIGN a clip that already belongs to a pill but lost its
            // schedule across a cold reboot. The HELLO ACK only restores its
            // node_id; without this the clip would sit assigned but never alarm.
            if (node_is_assigned(id) && schedule_source != NULL) {
                const week_schedule* sched = schedule_source(id);
                if (sched != NULL) {
                    assign_msg m;
                    m.type = MSG_ASSIGN;
                    m.seq = 0;   // stamped by reliable_send
                    m.node_id = (uint8_t)id;
                    m.schedule = *sched;
                    if (!fetch_time(&m.time)) {
                        memset(&m.time, 0, sizeof(m.time));
                    }
                    if (reliable_send(&tx, mac, &m, sizeof(m))) {
                        Serial.printf("[hub] re-assigned schedule to node #%d\n", id);
                    }
                }
            }
        }
    }

    // Drain ACKs from clips (confirming assign/blink/remove/time_response).
    uint8_t buf[COMMS_RING_MAX_PAYLOAD];
    uint8_t len;
    while (comms_ring_pop(&ack_ring, mac, buf, &len)) {
        if (len < sizeof(ack_msg)) {
            continue;
        }
        const ack_msg* a = (const ack_msg*)buf;
        reliable_on_ack(&tx, a->seq);
        Serial.print("[hub] ack from node #");
        Serial.print((int)a->node_id);
        Serial.print(" for msg ");
        Serial.print((int)a->msg_to_ack);
        Serial.print(" (");
        print_mac(mac);
        Serial.println(")");
    }

    // WiFi reconnect (throttled).
    if (WiFi.status() != WL_CONNECTED &&
        now - last_wifi_retry_ms >= HUB_WIFI_RETRY_INTERVAL_MS) {
        last_wifi_retry_ms = now;
        Serial.println("[hub] WiFi lost, reconnecting");
        WiFi.reconnect();
    }

    // If the AP moved us to a different channel, re-anchor the ESP-NOW peers
    // (they are pinned to the channel they were added on, so a stale peer would
    // silently stop delivering once the AP hops channels).
    if (WiFi.status() == WL_CONNECTED) {
        uint8_t cur = WiFi.channel();
        if (cur != channel) {
            Serial.printf("[hub] WiFi channel changed %d -> %d, re-anchoring peers\n",
                          (int)channel, (int)cur);
            channel = cur;

            esp_now_del_peer(ESPNOW_BROADCAST_MAC);
            esp_now_peer_info_t bcast;
            memset(&bcast, 0, sizeof(bcast));
            memcpy(bcast.peer_addr, ESPNOW_BROADCAST_MAC, 6);
            bcast.channel = channel;
            bcast.ifidx = WIFI_IF_STA;
            bcast.encrypt = false;
            if (esp_now_add_peer(&bcast) != ESP_OK) {
                Serial.println("[hub] re-add broadcast peer failed");
            }

            // Drop every registered node peer; the ensure_peer() loop below
            // re-adds them on the new channel.
            for (int id = 1; id <= MAX_NODES; id++) {
                uint8_t m[6];
                if (node_get_mac(id, m)) {
                    esp_now_del_peer(m);
                }
            }
        }
    }

    // Beacon: poll for new nodes.
    if (now - last_beacon_ms >= HUB_BEACON_INTERVAL_MS) {
        last_beacon_ms = now;
        beacon_msg m;
        m.type = MSG_BEACON;
        m.version = PROTOCOL_VERSION;
        esp_now_send(ESPNOW_BROADCAST_MAC, (const uint8_t*)&m, sizeof(m));
    }

    // Make sure every discovered node is an ESP-NOW peer so we can unicast it.
    for (int id = 1; id <= MAX_NODES; id++) {
        if (node_get_mac(id, mac)) {
            ensure_peer(mac);
        }
    }

    // Answer a pending time request (RTC correction). The response is reliable:
    // if the clip is momentarily unreachable it is retried until ACKed.
    while (comms_ring_pop(&time_ring, mac, NULL, NULL)) {
        hub_time t;
        if (fetch_time(&t)) {
            time_response_msg m;
            m.type = MSG_TIME_RESPONSE;
            m.seq = 0;   // stamped by reliable_send
            m.time = t;
            reliable_send(&tx, mac, &m, sizeof(m));
        }
        // If the clock isn't synced yet the node will re-request.
    }

    // Prune unassigned nodes that stopped announcing themselves.
    int gone = node_forget_stale(now, HUB_STALE_NODE_MS);
    if (gone) {
        Serial.printf("[hub] forgot %d stale node(s)\n", gone);
    }
}

bool hub_comms_assign(int node_id, const week_schedule* schedule) {
    uint8_t mac[6];
    if (!node_get_mac(node_id, mac)) {
        return false;
    }

    assign_msg m;
    m.type = MSG_ASSIGN;
    m.seq = 0;   // stamped by reliable_send
    m.node_id = (uint8_t)node_id;
    m.schedule = *schedule;
    if (!fetch_time(&m.time)) {
        memset(&m.time, 0, sizeof(m.time));
        Serial.println("[hub] warning: assigning without synced time");
    }

    return reliable_send(&tx, mac, &m, sizeof(m));
}

bool hub_comms_blink(int node_id, uint8_t seconds) {
    uint8_t mac[6];
    if (!node_get_mac(node_id, mac)) {
        return false;
    }
    blink_msg m;
    m.type = MSG_BLINK;
    m.seq = 0;   // stamped by reliable_send
    m.seconds = seconds;
    return reliable_send(&tx, mac, &m, sizeof(m));
}

bool hub_comms_remove(int node_id) {
    uint8_t mac[6];
    if (!node_get_mac(node_id, mac)) {
        return false;
    }
    remove_msg m;
    m.type = MSG_REMOVE;
    m.seq = 0;   // stamped by reliable_send
    m.node_id = (uint8_t)node_id;
    return reliable_send(&tx, mac, &m, sizeof(m));
}

void hub_comms_set_schedule_source(const week_schedule* (*cb)(int node_id)) {
    schedule_source = cb;
}

bool hub_comms_take_event(uint8_t* type, uint8_t* node_id, uint16_t* mv) {
    while (event_tail != event_head) {
        uint8_t  t = event_types[event_tail];
        uint8_t  n = event_node_ids[event_tail];
        uint8_t  s = event_seqs[event_tail];
        uint16_t m = event_mvs[event_tail];
        event_tail = (uint8_t)((event_tail + 1) % HUB_EVENT_QUEUE_SIZE);

        // De-duplicate: a repeated seq is a retransmission we already applied.
        bool dup = (n >= 1 && n <= MAX_NODES && s == adhlog.last_seq[n]);

        // ACK whether fresh or duplicate so the clip stops retrying.
        hub_ack_event(n, t, s);

        if (dup) {
            continue;   // already applied — skip to the next queued event
        }
        if (n >= 1 && n <= MAX_NODES) {
            adhlog.last_seq[n] = s;
            if (t == MSG_BATTERY) {
                last_battery_mv[n] = m;
                adherence_log_save();   // persist the de-dup watermark
            } else if (t == MSG_TAKEN || t == MSG_MISSED || t == MSG_BOX_OPEN) {
                // Persist a fresh adherence event (also saves the watermark).
                adherence_log_append(n, t, s, (uint32_t)time(NULL));
            }
        }

        if (type != NULL) {
            *type = t;
        }
        if (node_id != NULL) {
            *node_id = n;
        }
        if (mv != NULL) {
            *mv = m;
        }
        return true;
    }
    return false;
}

uint16_t hub_comms_battery_mv(int node_id) {
    if (node_id < 1 || node_id > MAX_NODES) {
        return 0;
    }
    return last_battery_mv[node_id];
}

uint16_t hub_comms_adherence_count(void) {
    return adhlog.count;
}

bool hub_comms_adherence_get(uint16_t index, uint8_t* node_id, uint8_t* type,
                             uint8_t* seq, uint32_t* timestamp) {
    if (index >= adhlog.count) {
        return false;
    }
    const adherence_log_entry_t* e =
        &adhlog.entries[(adhlog.start + index) % HUB_ADHERENCE_LOG_MAX];
    if (node_id != NULL)   *node_id   = e->node_id;
    if (type != NULL)      *type      = e->type;
    if (seq != NULL)       *seq       = e->seq;
    if (timestamp != NULL) *timestamp = e->timestamp;
    return true;
}

void hub_comms_adherence_clear(void) {
    // Drop all logged events but keep the de-dup watermark, so clearing the
    // server-facing log does not re-open the double-count window.
    adhlog.count = 0;
    adhlog.start = 0;
    adherence_log_save();
}
