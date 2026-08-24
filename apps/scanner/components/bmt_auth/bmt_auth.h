#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* Call once at boot, BEFORE bmt_scan starts receiving/verifying any ADV. */
void bmt_auth_init(void);

/* Verify a Tag ADV payload - uses the epoch key derived from BMT_TAG_MASTER_KEY,
 * with auto-detect + windowed resync (see the full explanation in bmt_auth.c).
 * tag_id is read from p.minor in the payload and is used to look up per-tag
 * epoch sync state. */
bool bmt_auth_verify_tag(uint16_t tag_id, const uint8_t* data, int len, uint16_t received_mac);

/* Epoch the Scanner is currently "locked" to for this tag (for display/debug).
 * Returns -1 if never locked (tag not seen yet, or just added and hasn't
 * verified successfully once). */
int32_t bmt_auth_get_locked_epoch(uint16_t tag_id);

/* Compute the HMAC for an OTA-beacon payload - uses the current key (NVS
 * value if a rotate from Gateway was received, otherwise the hardcoded
 * default). Compared manually at the caller. */
uint16_t bmt_auth_ota_beacon_hmac16(const uint8_t* data, size_t len);

/* [SECURITY] Gateway pushes a new HMAC beacon key over mesh (already encrypted
 * at the transport layer by the AppKey) - swap the in-use key and persist to
 * NVS so it survives a reboot. */
void bmt_auth_set_ota_beacon_key(const uint8_t* key, size_t len);
