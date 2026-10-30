#include <assert.h>
#include "bmt_ota.h"
#include "bmt_ota_key.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "psa/crypto.h"

#include "bmt_mesh.h"
#include "bmt_node_table.h"
#include "bmt_types.h"

/* [SECURITY] HMAC key that signs the scanner OTA-beacon: generated on
 * first boot, persisted in NVS, rotated every 24 h and pushed to every
 * configured scanner over mesh (AppKey-encrypted). */

static const char* TAG = "BMT_OTA";

#define BMT_OTA_NVS_NAMESPACE "bmt_ota"
#define BMT_OTA_NVS_KEY_BEACON_KEY "beacon_key"
#define BMT_OTA_KEY_ROTATE_INTERVAL_MS (24ULL * 60 * 60 * 1000)

static psa_key_id_t s_beacon_hmac_key_id = 0;
static uint8_t s_beacon_hmac_key_raw[16];

static void beacon_key_persist(const uint8_t* key)
{
	nvs_handle_t h;
	esp_err_t err = nvs_open(BMT_OTA_NVS_NAMESPACE, NVS_READWRITE, &h);
	if (err != ESP_OK)
	{
		ESP_LOGE(TAG, "[SECURITY] persist: nvs_open fail: %s", esp_err_to_name(err));
		return;
	}
	err = nvs_set_blob(h, BMT_OTA_NVS_KEY_BEACON_KEY, key, 16);
	if (err != ESP_OK)
		ESP_LOGE(TAG, "[SECURITY] persist: nvs_set_blob fail: %s", esp_err_to_name(err));
	else if ((err = nvs_commit(h)) != ESP_OK)
		ESP_LOGE(TAG, "[SECURITY] persist: nvs_commit fail: %s", esp_err_to_name(err));
	nvs_close(h);
}

static esp_err_t beacon_key_import(const uint8_t* key)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
	psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
	psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
	psa_set_key_bits(&attr, 8 * 16);

	/* Import into a NEW slot first, only swap after import succeeds - 
	 * rollback-safe: if the import fails, the old key
	 * (s_beacon_hmac_key_id + raw) is still usable. */
	psa_key_id_t new_id = 0;
	psa_status_t st = psa_import_key(&attr, key, 16, &new_id);
	if (st != PSA_SUCCESS)
	{
		ESP_LOGE(TAG, "[SECURITY] psa_import_key failed: %d - keeping the old key", (int)st);
		return ESP_FAIL;
	}

	psa_key_id_t old_id = s_beacon_hmac_key_id;
	s_beacon_hmac_key_id = new_id;
	memcpy(s_beacon_hmac_key_raw, key, 16);
	if (old_id != 0)
		psa_destroy_key(old_id);
	return ESP_OK;
}

static void beacon_hmac_key_init(void)
{
	psa_status_t st = psa_crypto_init();
	if (st != PSA_SUCCESS)
	{
		ESP_LOGE(TAG, "[SECURITY] psa_crypto_init failed: %d", (int)st);
		return;
	}

	uint8_t nvs_key[16];
	bool have_nvs_key = false;
	nvs_handle_t h;
	if (nvs_open(BMT_OTA_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK)
	{
		size_t len = sizeof(nvs_key);
		if (nvs_get_blob(h, BMT_OTA_NVS_KEY_BEACON_KEY, nvs_key, &len) == ESP_OK && len == sizeof(nvs_key))
		{
			have_nvs_key = true;
		}
		nvs_close(h);
	}

	if (have_nvs_key)
	{
		beacon_key_import(nvs_key);
		ESP_LOGI(TAG, "[SECURITY] OTA-beacon key loaded from NVS (key_id=%" PRIu32 ")", (uint32_t)s_beacon_hmac_key_id);
		return;
	}

	uint8_t new_key[16];
	esp_fill_random(new_key, sizeof(new_key));
	beacon_key_persist(new_key);
	beacon_key_import(new_key);
	ESP_LOGW(TAG, "[SECURITY] OTA-beacon key: generated RANDOM on first boot (not using the hardcoded key), key_id=%" PRIu32,
	         (uint32_t)s_beacon_hmac_key_id);
}

/* Generate a NEW random HMAC beacon key, persist to NVS, and push it
 * to every provisioned Scanner over mesh (the vendor message is
 * already encrypted by the AppKey). */
static void beacon_key_rotate_and_push(void)
{
	uint8_t new_key[16];
	esp_fill_random(new_key, sizeof(new_key));
	/* Import first - if it fails, do NOT persist and do NOT push, keep
	 * the old key so NVS / PSA / scanner do not drift apart (a gateway
	 * reboot would then load the wrong key). */
	if (beacon_key_import(new_key) != ESP_OK)
	{
		ESP_LOGE(TAG, "[SECURITY] Key rotate ABORTED - keeping the old key, not pushing");
		return;
	}
	beacon_key_persist(new_key);
	ESP_LOGW(TAG, "[SECURITY] OTA-beacon key ROTATED (key_id=%" PRIu32 ") - pushing to all scanners...",
	         (uint32_t)s_beacon_hmac_key_id);

	int pushed = 0;
	for (int i = 0; i < bmt_node_table_capacity(); i++)
	{
		bmt_node_t* n = bmt_node_table_get(i);
		if (!n || !n->used || !n->is_scan || !n->config_done)
			continue;
		esp_err_t e = bmt_mesh_publish(n->addr, BMT_OP_VND_OTA_KEY_PUSH, s_beacon_hmac_key_raw, 16);
		if (e == ESP_OK)
			pushed++;
		vTaskDelay(pdMS_TO_TICKS(300));
	}
	ESP_LOGW(TAG, "[SECURITY] Key rotate: pushed to %d scanner(s)", pushed);
}
void bmt_ota_push_beacon_key_to_node(uint16_t addr)
{
	if (s_beacon_hmac_key_id == 0)
		return;
	esp_err_t e = bmt_mesh_publish(addr, BMT_OP_VND_OTA_KEY_PUSH, s_beacon_hmac_key_raw, 16);
	ESP_LOGI(TAG, "[SECURITY] Pushed current beacon key to 0x%04x: %s", addr, e == ESP_OK ? "OK" : esp_err_to_name(e));
}

static void key_rotate_task(void* arg)
{
	(void)arg;
	while (1)
	{
		vTaskDelay(pdMS_TO_TICKS(BMT_OTA_KEY_ROTATE_INTERVAL_MS));
		beacon_key_rotate_and_push();
	}
}

void bmt_ota_start_key_rotation(void)
{
	assert(xTaskCreate(key_rotate_task, "bmt_key_rot", 3072, NULL, 3, NULL) == pdPASS);
}

uint16_t bmt_ota_key_hmac16(const uint8_t* data, size_t len)
{
	uint8_t mac[32];
	size_t mac_len = 0;
	psa_status_t st = psa_mac_compute(s_beacon_hmac_key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
	                                  data, len, mac, sizeof(mac), &mac_len);
	if (st != PSA_SUCCESS)
	{
		ESP_LOGE(TAG, "[SECURITY] psa_mac_compute failed: %d", (int)st);
		return 0;
	}
	return (uint16_t)((mac[0] << 8) | mac[1]);
}

/* Call once at boot (from main.c). Imports the HMAC key for the
 * OTA-beacon so it is ready before UART or RPC can trigger an OTA at
 * any moment. */
void bmt_ota_beacon_key_init(void)
{
	beacon_hmac_key_init();
}
