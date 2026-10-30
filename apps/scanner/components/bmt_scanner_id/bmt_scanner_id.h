#pragma once

#include <stdint.h>

/* Dynamic scanner ID (1-8) - persisted in NVS, set via UART 'i'.
 * Defaults to 0x01 when NVS has no value (first flash). */

/* Load the ID from NVS. Call once at boot, before anything reads it. */
void bmt_scanner_id_load(void);

uint8_t bmt_scanner_id(void);

/* Persist a new ID to NVS and use it from now on. */
void bmt_scanner_id_set(uint8_t id);
