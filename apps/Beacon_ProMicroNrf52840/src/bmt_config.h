#pragma once

#include <stdint.h>

/* System UUID - IDENTICAL across all tags (ESP32 + nRF52840 + iPhone).
 * Scanner checks the first 4 bytes (AB 00 00 00) to recognise
 * "our system's tag". */
static const uint8_t BMT_SYSTEM_UUID[16] = {
    0xAB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
#define BMT_TAG_MINOR 0x0001   /* tag ID - change per device */
#define BMT_TAG_TX_POWER (-53) /* calibrate: measure RSSI at 1 m, then set */
/* (BMT_TAG_MAJOR PERSON/ASSET removed - the payload now uses those 2 bytes for battery %.) */

/* [POWER] Random ADV interval range - avoids collisions with other
 * tags. Raised from 450-550 ms to ~900-1100 ms to reduce battery
 * drain: the radio transmits about half as often, so average current
 * drops and battery/diode sag is smaller. Scanner updates positions
 * about twice as slowly (~1 s), acceptable for a worn tag. Lower
 * again for faster localisation at the cost of battery. */
#define BMT_ADV_INTERVAL_MIN_MS 900
#define BMT_ADV_INTERVAL_MAX_MS 1100

/* Actual radio TX power is distinct from BMT_TAG_TX_POWER above (the
 * "declared" value in the payload the Scanner uses for distance
 * estimation). On Zephyr / nRF52 it is set via Kconfig
 * (CONFIG_BT_CTLR_TX_PWR_*), not a runtime API like ESP-IDF. */
