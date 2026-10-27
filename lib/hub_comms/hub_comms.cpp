#include "hub_comms.h"
#include "node_management.h"

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

// Shared with the ESP-NOW receive callback, which runs in the ESP-NOW task. The
// callback only fills these buffers; the main loop drains them (keeps all
// node_management access on one thread).
static volatile bool pending_hello = false;
static uint8_t pending_hello_mac[6];
static volatile bool pending_time_request = false;
static uint8_t pending_time_mac[6];

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

// --- ESP-NOW callbacks (ESP-NOW task; keep them short, no LCD / delays / sends) ---

static void on_data_recv(const uint8_t* mac, const uint8_t* data, int len) {
    if (len < 1) {
        return;
    }
    switch (data[0]) {
    case MSG_HELLO:
        memcpy(pending_hello_mac, mac, 6);
        pending_hello = true;   // set after the MAC is written
        break;
    case MSG_TIME_REQUEST:
        memcpy(pending_time_mac, mac, 6);
        pending_time_request = true;
        break;
    default:
        break;   // the hub doesn't receive the hub->node message types
    }
}

static void on_data_sent(const uint8_t* mac, esp_now_send_status_t status) {
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

    // Drain a pending HELLO.
    if (pending_hello) {
        pending_hello = false;
        uint8_t mac[6];
        memcpy(mac, pending_hello_mac, 6);
        int id = node_register(mac, now);
        if (id) {
            Serial.print("[hub] node #");
            Serial.print(id);
            Serial.print(" (");
            print_mac(mac);
            Serial.println(") wants to connect");
        }
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
        uint8_t mac[6];
        if (node_get_mac(id, mac)) {
            ensure_peer(mac);
        }
    }

    // Answer a pending time request (RTC correction).
    if (pending_time_request) {
        pending_time_request = false;
        uint8_t mac[6];
        memcpy(mac, pending_time_mac, 6);
        hub_time t;
        if (fetch_time(&t)) {
            time_response_msg m;
            m.type = MSG_TIME_RESPONSE;
            m.time = t;
            esp_now_send(mac, (const uint8_t*)&m, sizeof(m));
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
    m.node_id = (uint8_t)node_id;
    m.schedule = *schedule;
    if (!fetch_time(&m.time)) {
        memset(&m.time, 0, sizeof(m.time));
        Serial.println("[hub] warning: assigning without synced time");
    }

    return esp_now_send(mac, (const uint8_t*)&m, sizeof(m)) == ESP_OK;
}

bool hub_comms_blink(int node_id, uint8_t seconds) {
    uint8_t mac[6];
    if (!node_get_mac(node_id, mac)) {
        return false;
    }
    blink_msg m;
    m.type = MSG_BLINK;
    m.seconds = seconds;
    return esp_now_send(mac, (const uint8_t*)&m, sizeof(m)) == ESP_OK;
}

bool hub_comms_remove(int node_id) {
    uint8_t mac[6];
    if (!node_get_mac(node_id, mac)) {
        return false;
    }
    remove_msg m;
    m.type = MSG_REMOVE;
    m.node_id = (uint8_t)node_id;
    return esp_now_send(mac, (const uint8_t*)&m, sizeof(m)) == ESP_OK;
}

bool hub_comms_now(hub_time* out) {
    return fetch_time(out);
}
