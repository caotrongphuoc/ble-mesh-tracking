#pragma once

#include <stdint.h>
#include "esp_bt.h"

/* System UUID - IDENTICAL across all tags (ESP32 + iPhone).
 * Scanner checks the first 4 bytes (AB 00 00 00) to recognise
 * "our system's tag". */
static const uint8_t BMT_SYSTEM_UUID[16] = {
    0xAB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
#define BMT_TAG_MAJOR 0x0001   /* change per tag category */
#define BMT_TAG_MINOR 0x0001   /* change per physical device */
#define BMT_TAG_TX_POWER (-53) /* calibrate: measure RSSI at 1 m, then set */

/* Real radio TX power (affects range and battery life).
 * ESP_PWR_LVL_N0 = 0 dBm -> ~10-15 m indoor, fits this system. */
#define BMT_TAG_RADIO_PWR ESP_PWR_LVL_N0

/* Random ADV interval within this range to avoid collisions with
 * other tags. */
#define BMT_ADV_INTERVAL_MIN_MS 450
#define BMT_ADV_INTERVAL_MAX_MS 550
