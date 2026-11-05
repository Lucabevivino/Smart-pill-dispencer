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

// --- tuning ---
#define HUB_TICK_INTERVAL_MS       100     // min gap between housekeeping runs
#define HUB_BEACON_INTERVAL_MS     5000    // how often to poll for new nodes
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

// Adherence events (MSG_TAKEN / MSG_MISSED / MSG_BOX_OPEN) from clips. A small
// ring buffer (single producer: the ESP-NOW callback; single consumer: the main
// loop) with the sender's seq, so retransmissions can be de-duplicated and ACKed.
#define HUB_EVENT_QUEUE_SIZE 8
static volatile uint8_t event_types[HUB_EVENT_QUEUE_SIZE];
static volatile uint8_t event_node_ids[HUB_EVENT_QUEUE_SIZE];
static volatile uint8_t event_seqs[HUB_EVENT_QUEUE_SIZE];
static volatile uint8_t event_head = 0;   // next write slot (callback)
static volatile uint8_t event_tail = 0;   // next read slot (main loop)

// Last adherence seq accepted per node (1-based id). A repeated seq is a
// retransmission: we re-ACK it but do not apply it again.
static uint8_t last_adherence_seq[MAX_NODES + 1];

// Reliable outbound queue (assign / blink / remove / time response). Plain RAM:
// the hub is mains-powered and its durable state lives in NVS already.
static reliable_tx_t tx;

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
static void queue_event(uint8_t type, uint8_t node_id, uint8_t seq) {
    uint8_t next = (uint8_t)((event_head + 1) % HUB_EVENT_QUEUE_SIZE);
    if (next == event_tail) {
        return;   // queue full — drop the newest event rather than clobber one
    }
    event_types[event_head] = type;
    event_node_ids[event_head] = node_id;
    event_seqs[event_head] = seq;
    event_head = next;
}

// --- ESP-NOW callbacks (ESP-NOW task; keep them short, no LCD / delays / sends) ---

static void on_data_recv(const uint8_t* mac, const uint8_t* data, int len) {
    if (len < 1) {
        return;
    }
    switch (data[0]) {
    case MSG_HELLO:
        comms_ring_push(&hello_ring, mac, data, (uint8_t)len);
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
                        ((const taken_msg*)data)->seq);
        }
        break;
    case MSG_MISSED:
        if (len >= (int)sizeof(missed_msg)) {
            queue_event(MSG_MISSED, ((const missed_msg*)data)->node_id,
                        ((const missed_msg*)data)->seq);
        }
        break;
    case MSG_BOX_OPEN:
        if (len >= (int)sizeof(box_open_msg)) {
            queue_event(MSG_BOX_OPEN, ((const box_open_msg*)data)->node_id,
                        ((const box_open_msg*)data)->seq);
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

    reliable_init(&tx, reliable_send_fn);

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

bool hub_comms_now(hub_time* out) {
    return fetch_time(out);
}

bool hub_comms_take_event(uint8_t* type, uint8_t* node_id) {
    while (event_tail != event_head) {
        uint8_t t = event_types[event_tail];
        uint8_t n = event_node_ids[event_tail];
        uint8_t s = event_seqs[event_tail];
        event_tail = (uint8_t)((event_tail + 1) % HUB_EVENT_QUEUE_SIZE);

        // De-duplicate: a repeated seq is a retransmission we already applied.
        bool dup = (n >= 1 && n <= MAX_NODES && s == last_adherence_seq[n]);

        // ACK whether fresh or duplicate so the clip stops retrying.
        hub_ack_event(n, t, s);

        if (dup) {
            continue;   // already applied — skip to the next queued event
        }
        if (n >= 1 && n <= MAX_NODES) {
            last_adherence_seq[n] = s;
        }
        if (type != NULL) {
            *type = t;
        }
        if (node_id != NULL) {
            *node_id = n;
        }
        return true;
    }
    return false;
}
