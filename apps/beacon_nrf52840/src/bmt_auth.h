#pragma once

#include <stddef.h>
#include <stdint.h>

/* Call once at boot, BEFORE the first build_adv_data(). */
void bmt_auth_init(void);

/* Compute HMAC-16 over the payload - used when building an ADV frame
 * on each sequence bump. */
uint16_t bmt_auth_hmac16(const uint8_t* data, size_t len);
