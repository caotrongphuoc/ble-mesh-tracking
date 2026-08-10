#include "bmt_auth.h"

#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "psa/crypto.h"

static const char* TAG = "BMT_AUTH";

/* [SECURITY] Master key — NOT used directly to sign payloads any more.
 * It derives one key per epoch (rotates over time, TOTP-style,
 * receiver-less — the Tag has no mesh/WiFi so it cannot receive a
 * rotated key pushed down like the Scanner's OTA-beacon key). Even if
 * this key is stolen from flash, the attacker still has to derive the
 * current epoch key — a single static key cannot be replayed forever. */
static const uint8_t BMT_TAG_MASTER_KEY[16] = {
    0x76, 0x9F, 0xD5, 0x0D, 0xDF, 0x25, 0xA7, 0x7B,
    0x12, 0x7F, 0xDB, 0xC0, 0xCB, 0xFF, 0xF8, 0xB1};

/* Epoch tick = 1 h. The Tag has NO RTC or WiFi, so the epoch is a LOCAL
 * counter measured from boot (esp_timer_get_time(), not wall-clock) — it
 * resets to 0 on every power loss (battery swap). The Scanner absorbs
 * this drift via windowed resync — see bmt_auth_verify_tag() on the
 * Scanner side. MUST EXACTLY MATCH the value used on the Scanner. */
#define BMT_EPOCH_TICK_SEC 3600

static psa_key_id_t s_master_key_id = 0;
static psa_key_id_t s_epoch_key_id = 0;
static uint16_t s_cached_epoch = 0xFFFF; /* invalid value -> forces derivation on first use */

static uint16_t current_epoch(void)
{
	return (uint16_t)(esp_timer_get_time() / (1000000LL * BMT_EPOCH_TICK_SEC));
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
		ESP_LOGE(TAG, "derive epoch key failed: %d", (int)st);
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
		ESP_LOGE(TAG, "import epoch key failed: %d", (int)st);
		s_epoch_key_id = 0;
		return;
	}
	s_cached_epoch = epoch;
	ESP_LOGI(TAG, "[SECURITY] epoch key rotated -> epoch=%u", epoch);
}

void bmt_auth_init(void)
{
	psa_status_t st = psa_crypto_init();
	if (st != PSA_SUCCESS)
	{
		ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)st);
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
		ESP_LOGE(TAG, "psa_import_key failed: %d", (int)st);
		return;
	}
	ensure_epoch_key(current_epoch());
	ESP_LOGI(TAG, "HMAC master key imported OK (key_id=%" PRIu32 ")", (uint32_t)s_master_key_id);
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
		ESP_LOGE(TAG, "psa_mac_compute failed: %d", (int)st);
		return 0;
	}
	return (uint16_t)((mac[0] << 8) | mac[1]);
}
