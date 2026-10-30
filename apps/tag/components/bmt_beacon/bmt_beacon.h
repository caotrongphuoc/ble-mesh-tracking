#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "bmt_protocol.h"

/* Initialise Bluetooth, register the GAP callback, create the 500 ms
 * sequence timer, and start advertising. MUST be called after
 * bmt_auth_init(). */
esp_err_t bmt_beacon_start(void);

/* Getters used by UART status. */
uint8_t bmt_beacon_sequence(void);
uint16_t bmt_beacon_last_mac16(void);
bool bmt_beacon_is_active(void);
