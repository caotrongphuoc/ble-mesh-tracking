#include "bmt_auth.h"

#include <inttypes.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "psa/crypto.h"

LOG_MODULE_REGISTER(bmt_auth, LOG_LEVEL_INF);

/* [SECURITY] Master key - NOT used directly to sign the payload. It
 * derives a separate key per epoch (rotated over time, TOTP-style,
 * receiver-less: Tag has no mesh / WiFi, so it cannot receive a
 * pushed rotation like the OTA-beacon key does on Scanner). If this
 * key is stolen from flash, the attacker still has to recompute the
 * current epoch key - they cannot "replay a single static key
 * forever".
 * MUST match BMT_TAG_MASTER_KEY on the ESP32 Tag byte-for-byte. */
static const uint8_t BMT_TAG_MASTER_KEY[16] = {
    0x76, 0x9F, 0xD5, 0x0D, 0xDF, 0x25, 0xA7, 0x7B,
    0x12, 0x7F, 0xDB, 0xC0, 0xCB, 0xFF, 0xF8, 0xB1};

/* Epoch tick = 1 h. Tag has no RTC / WiFi, so the epoch is a LOCAL
 * counter measured from boot (k_uptime_get(), not wall-clock) - 
 * resets to 0 on every power loss (CR2032 swap). Scanner deals with
 * the drift via windowed resync. MUST match the value on the
 * Scanner (ESP32). */
#define BMT_EPOCH_TICK_SEC 3600

static psa_key_id_t s_master_key_id = 0;
static psa_key_id_t s_epoch_key_id = 0;
static uint16_t s_cached_epoch = 0xFFFF; /* invalid value -> forces derivation on first use */

static uint16_t current_epoch(void)
{
	/* k_uptime_get() returns milliseconds (int64_t) since boot - 
	 * different from ESP-IDF's esp_timer_get_time() which is
	 * microseconds. Adjust the scale factor accordingly. */
	return (uint16_t)(k_uptime_get() / (1000LL * BMT_EPOCH_TICK_SEC));
}

static void ensure_epoch_key(uint16_t epoch)
{
	if (epoch == s_cached_epoch && s_epoch_key_id != 0)
		return;

	uint8_t derived[32];
	size_t derived_len = 0;
	uint8_t epoch_le[2] = {(uint8_t)(epoch & 0xFF), (uint8_t)(epoch >> 8)};

	psa_status_t st = psa_mac_compute(s_master_key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
	                                  epoch_le, sizeof(epoch_le), derived, sizeof(derived), &derived_len);
	if (st != PSA_SUCCESS)
	{
		LOG_ERR("derive epoch key failed: %d", (int)st);
		return;
	}

	if (s_epoch_key_id != 0)
		psa_destroy_key(s_epoch_key_id);

	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
	psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
	psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
	psa_set_key_bits(&attr, 128);

	st = psa_import_key(&attr, derived, 16, &s_epoch_key_id);
	if (st != PSA_SUCCESS)
	{
		LOG_ERR("import epoch key failed: %d", (int)st);
		s_epoch_key_id = 0;
		return;
	}
	s_cached_epoch = epoch;
	LOG_INF("[SECURITY] epoch key rotated -> epoch=%u", epoch);
}

void bmt_auth_init(void)
{
	psa_status_t st = psa_crypto_init();
	if (st != PSA_SUCCESS)
	{
		LOG_ERR("psa_crypto_init failed: %d", (int)st);
		return;
	}

	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
	psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
	psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
	psa_set_key_bits(&attr, 8 * sizeof(BMT_TAG_MASTER_KEY));

	st = psa_import_key(&attr, BMT_TAG_MASTER_KEY, sizeof(BMT_TAG_MASTER_KEY), &s_master_key_id);
	if (st != PSA_SUCCESS)
	{
		LOG_ERR("psa_import_key failed: %d", (int)st);
		return;
	}
	ensure_epoch_key(current_epoch());
	LOG_INF("HMAC master key imported OK (key_id=%" PRIu32 ")", (uint32_t)s_master_key_id);
}

uint16_t bmt_auth_hmac16(const uint8_t* data, size_t len)
{
	ensure_epoch_key(current_epoch());

	uint8_t mac[32];
	size_t mac_len = 0;

	psa_status_t st = psa_mac_compute(s_epoch_key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
	                                  data, len, mac, sizeof(mac), &mac_len);
	if (st != PSA_SUCCESS)
	{
		LOG_ERR("psa_mac_compute failed: %d", (int)st);
		return 0;
	}
	return (uint16_t)((mac[0] << 8) | mac[1]);
}
