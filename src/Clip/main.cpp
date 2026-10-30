#include <Arduino.h>
#include "clip_comms.h"

void setup() {
    Serial.begin(115200);

    clip_comms_begin();          // ESP-NOW + broadcast peer
    clip_comms_find_hub(30000);  // channel-scan until the hub's beacon is heard
    Serial.println("Hub found!");
}

void loop() {
    clip_comms_loop();
}
