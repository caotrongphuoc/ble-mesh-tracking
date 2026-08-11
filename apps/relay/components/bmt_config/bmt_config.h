#pragma once

#include <stdint.h>

/* UUID prefix "RELAY" (0x52,0x45,0x4C,0x41,0x59) + byte 15 = relay ID (0x01, 0x02...) */
static const uint8_t BMT_RELAY_UUID[16] = {
    0x52, 0x45, 0x4C, 0x41, 0x59, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};

/* User config — edit before building. See docs/00-quickstart.md. */
#define BMT_WIFI_SSID "YOUR_WIFI_SSID"
#define BMT_WIFI_PASS "YOUR_WIFI_PASSWORD"
/* Replace 192.168.1.100 with your OTA server IP on the LAN. */
#define BMT_OTA_RELAY_URL "https://192.168.1.100:8443/Relay.bin"
#define BMT_OTA_SERVER_CN "bmt-tb.local"
#define BMT_OTA_WIFI_TIMEOUT_MS 30000

#define BMT_RELAY_NVS_NAMESPACE "bmt_relay"
