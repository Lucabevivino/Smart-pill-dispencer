#include "clip_comms.h"
#include "protocol.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>

// --- tuning ---
#define CLIP_SCAN_START_CHANNEL 1
#define CLIP_SCAN_END_CHANNEL   13
#define CLIP_SCAN_DWELL_MS      600

// Discovery state, shared with the ESP-NOW receive callback (which runs in the
// ESP-NOW task). The callback only writes these; the main loop reads them.
static uint8_t hub_mac[6];
static uint8_t channel = CLIP_SCAN_START_CHANNEL;
static volatile bool hub_found = false;
static bool ready = false;   // true once ESP-NOW + broadcast peer are up

// --- helpers ---

static void print_mac(const uint8_t* mac) {
    Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// --- ESP-NOW callbacks (ESP-NOW task; keep them short, no Serial / delays) ---

static void on_data_recv(const uint8_t* mac, const uint8_t* data, int len) {
    if (len < 1) {
        return;
    }
    if (data[0] == MSG_BEACON) {
        memcpy(hub_mac, mac, 6);
        hub_found = true;   // set after the MAC is written
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
        return true;
    }

    Serial.println("[clip] hub not found");
    return false;
}

void clip_comms_loop(void) {
    // Reserved for later phases (HELLO retries, time requests, ...).
}

bool clip_comms_is_connected(void) {
    return hub_found;
}

void clip_comms_hub_mac(uint8_t out[6]) {
    memcpy(out, hub_mac, 6);
}

uint8_t clip_comms_channel(void) {
    return channel;
}
