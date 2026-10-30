#pragma once

#include <stddef.h>
#include <stdint.h>

/* Private to the gateway bmt_ota component. Public key API
 * (init / rotation / push) is in bmt_ota.h. */

/* HMAC-SHA256 with the current OTA-beacon key, truncated to 16 bits.
 * Returns 0 on failure. */
uint16_t bmt_ota_key_hmac16(const uint8_t* data, size_t len);
