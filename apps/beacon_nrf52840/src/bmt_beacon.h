#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Create the 500 ms sequence timer and start advertising.
 * MUST be called AFTER a successful bt_enable() and after
 * bmt_auth_init() has run. */
int bmt_beacon_start(void);

/* Getters (debug / status). */
uint8_t bmt_beacon_sequence(void);
uint16_t bmt_beacon_last_mac16(void);
bool bmt_beacon_is_active(void);
