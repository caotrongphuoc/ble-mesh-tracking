#include "bmt_ota.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_random.h"
#include "esp_system.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "psa/crypto.h"
#include "host/ble_hs.h"

#include "bmt_config.h"
#include "bmt_mesh.h"
#include "bmt_node_table.h"
#include "bmt_thingsboard.h"
#include "bmt_types.h"

static const char* TAG = "BMT_OTA";

/* [SECURITY] CA embedded in firmware to verify the HTTPS OTA server —
 * shares one CA with MQTTS (same physical server). Previously OTA ran
 * over plain HTTP: anyone on the LAN could download the .bin and read
 * plaintext secrets (WiFi pass, TB token, ...) without touching a
 * board. This is the fix for that hole. */
extern const uint8_t bmt_ota_ca_pem_start[] asm("_binary_ota_ca_pem_start");
extern const uint8_t bmt_ota_ca_pem_end[] asm("_binary_ota_ca_pem_end");

#define BMT_OTA_HTTP_TIMEOUT_MS 15000
#define BMT_OTA_NODE_GAP_MS 90000        /* 90 s per node: enough to download ~840 KB and reboot */
#define BMT_OTA_BEACON_DURATION_MS 15000 /* 15 s: enough GAP scan cycles for the scanner to catch the beacon */

static atomic_bool s_running = false;

static void print_sha256_hex(const char* label, const uint8_t* sha, size_t len)
{
	printf("%s", label);
	for (size_t i = 0; i < len; i++)
		printf("%02x", sha[i]);
	printf("\n");
}
static void self_update_task(void* arg)
{
	(void)arg;
	printf("\n[OTA] ===== Gateway self-update =====\n");
	printf("[OTA] URL: %s\n", BMT_OTA_GATEWAY_URL);

	esp_http_client_config_t http_cfg = {
	    .url = BMT_OTA_GATEWAY_URL,
	    .timeout_ms = BMT_OTA_HTTP_TIMEOUT_MS,
	    .cert_pem = (const char*)bmt_ota_ca_pem_start,
	    .cert_len = (size_t)(bmt_ota_ca_pem_end - bmt_ota_ca_pem_start),
	    .common_name = BMT_OTA_SERVER_CN,
	};
	esp_https_ota_config_t ota_cfg = {.http_config = &http_cfg};
	esp_https_ota_handle_t ota_handle = NULL;

	esp_err_t err = esp_https_ota_begin(&ota_cfg, &ota_handle);
	if (err != ESP_OK)
	{
		printf("[OTA] esp_https_ota_begin FAILED: %s\n", esp_err_to_name(err));
		goto self_update_fail;
	}

	esp_app_desc_t new_desc;
	err = esp_https_ota_get_img_desc(ota_handle, &new_desc);
	if (err != ESP_OK)
	{
		printf("[OTA] esp_https_ota_get_img_desc FAILED: %s\n", esp_err_to_name(err));
		esp_https_ota_abort(ota_handle);
		goto self_update_fail;
	}

	const esp_app_desc_t* cur_desc = esp_app_get_description();

	print_sha256_hex("[OTA] Node   SHA256: ", cur_desc->app_elf_sha256, sizeof(cur_desc->app_elf_sha256));
	print_sha256_hex("[OTA] Server SHA256: ", new_desc.app_elf_sha256, sizeof(new_desc.app_elf_sha256));

	if (memcmp(new_desc.app_elf_sha256, cur_desc->app_elf_sha256, sizeof(new_desc.app_elf_sha256)) == 0)
	{
		printf("[OTA] SHA256 match — node firmware is IDENTICAL to server firmware, skip.\n");
		esp_https_ota_abort(ota_handle);
		atomic_store(&s_running, false);
		vTaskDelete(NULL);
		return;
	}
	printf("[OTA] SHA256 differ — node firmware DIFFERS from server firmware, checking version...\n");

	printf("[OTA] Node   version: %s\n", cur_desc->version);
	printf("[OTA] Server version: %s\n", new_desc.version);

	if (strncmp(new_desc.version, cur_desc->version, sizeof(new_desc.version)) <= 0)
	{
		printf("[OTA] Server version is NOT newer — skip, no downgrade.\n");
		esp_https_ota_abort(ota_handle);
		atomic_store(&s_running, false);
		vTaskDelete(NULL);
		return;
	}

	printf("[OTA] Server version is NEWER -> flashing firmware...\n");
	while (1)
	{
		err = esp_https_ota_perform(ota_handle);
		if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS)
			break;
	}

	if (err == ESP_OK && esp_https_ota_is_complete_data_received(ota_handle))
	{
		err = esp_https_ota_finish(ota_handle);
	}
	else
	{
		esp_https_ota_abort(ota_handle);
		if (err == ESP_OK)
			err = ESP_FAIL;
	}

	if (err == ESP_OK)
	{
		printf("[OTA] ===== OTA SUCCESS — rebooting =====\n");
		bmt_tb_pub_gateway_ota_result(true);
		vTaskDelay(pdMS_TO_TICKS(1000));
		esp_restart();
	}

self_update_fail:
	bmt_tb_pub_gateway_ota_result(false);
	printf("[OTA] Gateway self-update FAILED: %s\n", esp_err_to_name(err));
	atomic_store(&s_running, false);
	vTaskDelete(NULL);
}

esp_err_t bmt_ota_gateway_self_update(void)
{
	bool expected = false;
	if (!atomic_compare_exchange_strong(&s_running, &expected, true))
	{
		ESP_LOGW(TAG, "OTA already running");
		return ESP_ERR_INVALID_STATE;
	}
	if (xTaskCreate(self_update_task, "bmt_ota_self", 8192, NULL, 4, NULL) != pdPASS)
	{
		atomic_store(&s_running, false);
		return ESP_ERR_NO_MEM;
	}
	return ESP_OK;
}
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

	/* Import into a NEW slot first, only swap after import succeeds —
	 * rollback-safe: if the import fails, the old key
	 * (s_beacon_hmac_key_id + raw) is still usable. */
	psa_key_id_t new_id = 0;
	psa_status_t st = psa_import_key(&attr, key, 16, &new_id);
	if (st != PSA_SUCCESS)
	{
		ESP_LOGE(TAG, "[SECURITY] psa_import_key failed: %d — keeping the old key", (int)st);
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
	/* Import first — if it fails, do NOT persist and do NOT push, keep
	 * the old key so NVS / PSA / scanner do not drift apart (a gateway
	 * reboot would then load the wrong key). */
	if (beacon_key_import(new_key) != ESP_OK)
	{
		ESP_LOGE(TAG, "[SECURITY] Key rotate ABORTED — keeping the old key, not pushing");
		return;
	}
	beacon_key_persist(new_key);
	ESP_LOGW(TAG, "[SECURITY] OTA-beacon key ROTATED (key_id=%" PRIu32 ") — pushing to all scanners...",
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
	xTaskCreate(key_rotate_task, "bmt_key_rot", 3072, NULL, 3, NULL);
}

static uint16_t beacon_hmac16(const uint8_t* data, size_t len)
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

/* Beacon format: Flags + Manufacturer Specific (CID Espressif + "BMT" + 0xFA + target + mac16) */
static uint8_t s_beacon_raw[14] = {
    0x02, 0x01, 0x06, /* Flags: LE General Discoverable */
    0x0A, 0xFF,       /* Manufacturer Specific, length=10 */
    0xE5, 0x02,       /* CID Espressif (little-endian) */
    'B', 'M', 'T',    /* Magic "BMT" */
    0xFA,             /* Command: OTA trigger */
    0x01,             /* target_type — filled in before send */
    0x00, 0x00,       /* mac16 — HMAC-16 filled in before send */
};

static int adv_gap_event(struct ble_gap_event* event, void* arg)
{
	(void)arg;
	if (event->type == BLE_GAP_EVENT_ADV_COMPLETE)
		ESP_LOGI(TAG, "[OTA] beacon adv complete, reason=%d", event->adv_complete.reason);
	return 0;
}

static esp_err_t beacon_send(uint8_t target_type)
{
	s_beacon_raw[11] = target_type;
	uint16_t mac = beacon_hmac16(&s_beacon_raw[7], 5); /* magic(3)+cmd(1)+target(1) */
	s_beacon_raw[12] = (uint8_t)(mac >> 8);
	s_beacon_raw[13] = (uint8_t)(mac & 0xFF);

	int rc = ble_gap_adv_set_data(s_beacon_raw, sizeof(s_beacon_raw));
	if (rc != 0)
	{
		ESP_LOGE(TAG, "[OTA] ble_gap_adv_set_data FAILED rc=%d", rc);
		return ESP_FAIL;
	}

	struct ble_gap_adv_params adv_params = {0};
	adv_params.conn_mode = BLE_GAP_CONN_MODE_NON;
	adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
	adv_params.itvl_min = 32; /* 20ms = 32 * 0.625ms */
	adv_params.itvl_max = 64; /* 40ms */

	rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, adv_gap_event, NULL);
	if (rc != 0)
	{
		ESP_LOGE(TAG, "[OTA] ble_gap_adv_start FAILED rc=%d — the mesh host may already be advertising, "
		              "falling back to mesh unicast",
		         rc);
		return ESP_FAIL;
	}

	printf("[OTA] NimBLE beacon broadcasting (target=0x%02x, mac16=0x%04x, %ds)...\n",
	       target_type, mac, BMT_OTA_BEACON_DURATION_MS / 1000);
	vTaskDelay(pdMS_TO_TICKS(BMT_OTA_BEACON_DURATION_MS));
	ble_gap_adv_stop();
	printf("[OTA] NimBLE beacon stopped\n");
	return ESP_OK;
}
typedef struct
{
	uint8_t node_type;
	bool is_scan_filter;
} distribute_arg_t;

static void distribute_task(void* arg)
{
	distribute_arg_t d = *(distribute_arg_t*)arg;
	uint8_t target_type = d.node_type;
	bool is_scan = d.is_scan_filter;
	const char* type_str = is_scan ? "SCANNER" : "RELAY";

	int node_count = 0;
	for (int i = 0; i < bmt_node_table_capacity(); i++)
	{
		const bmt_node_t* n = bmt_node_table_get(i);
		if (!n || !n->used || !n->config_done)
			continue;
		if (is_scan && !n->is_scan)
			continue;
		if (!is_scan && !n->is_relay)
			continue;
		node_count++;
	}
	if (node_count == 0)
	{
		printf("[OTA] No configured %s nodes found\n", type_str);
		atomic_store(&s_running, false);
		vTaskDelete(NULL);
		return;
	}
	printf("[OTA] Found %d %s node(s)\n", node_count, type_str);

	if (is_scan)
	{
		printf("[OTA] Broadcasting NimBLE beacon (%ds) — all %d scanners OTA simultaneously\n",
		       BMT_OTA_BEACON_DURATION_MS / 1000, node_count);
		if (beacon_send(target_type) == ESP_OK)
		{
			printf("[OTA] Waiting %ds for all scanners to download + reboot...\n", BMT_OTA_NODE_GAP_MS / 1000);
			vTaskDelay(pdMS_TO_TICKS(BMT_OTA_NODE_GAP_MS));
			printf("[OTA] ===== Scanner OTA complete =====\n");
			atomic_store(&s_running, false);
			vTaskDelete(NULL);
			return;
		}
		printf("[OTA] Beacon broadcast failed — falling back to mesh unicast (slower but reliable)\n");
	}

	int idx = 0;
	for (int i = 0; i < bmt_node_table_capacity(); i++)
	{
		bmt_node_t* n = bmt_node_table_get(i);
		if (!n || !n->used || !n->config_done)
			continue;
		if (is_scan && !n->is_scan)
			continue;
		if (!is_scan && !n->is_relay)
			continue;

		idx++;
		printf("\n[OTA] -- %s %d/%d: 0x%04x (%s) --\n", type_str, idx, node_count, n->addr, n->name);

		for (int retry = 0; retry < 5; retry++)
		{
			esp_err_t e = bmt_mesh_publish(n->addr, BMT_OP_VND_OTA_TRIGGER, &target_type, sizeof(target_type));
			printf("[OTA] TRIGGER -> 0x%04x [%d/5]: %s\n", n->addr, retry + 1,
			       e == ESP_OK ? "sent" : esp_err_to_name(e));
			vTaskDelay(pdMS_TO_TICKS(500));
		}
		printf("[OTA] %s 0x%04x triggered — waiting %ds...\n", type_str, n->addr, BMT_OTA_NODE_GAP_MS / 1000);
		vTaskDelay(pdMS_TO_TICKS(BMT_OTA_NODE_GAP_MS));
	}

	printf("\n[OTA] ===== All %s nodes triggered! =====\n", type_str);
	atomic_store(&s_running, false);
	vTaskDelete(NULL);
}

static esp_err_t start_distribute(bool is_scan_filter, uint8_t node_type)
{
	bool expected = false;
	if (!atomic_compare_exchange_strong(&s_running, &expected, true))
	{
		ESP_LOGW(TAG, "OTA already running");
		return ESP_ERR_INVALID_STATE;
	}
	static distribute_arg_t s_arg; /* static: outlives the caller, the task reads it once at start */
	s_arg.is_scan_filter = is_scan_filter;
	s_arg.node_type = node_type;
	if (xTaskCreate(distribute_task, "bmt_ota_dst", 4096, &s_arg, 4, NULL) != pdPASS)
	{
		atomic_store(&s_running, false);
		return ESP_ERR_NO_MEM;
	}
	return ESP_OK;
}

esp_err_t bmt_ota_trigger_all_scanners(void)
{
	return start_distribute(true, BMT_NODE_TYPE_SCANNER);
}
esp_err_t bmt_ota_trigger_all_relays(void)
{
	return start_distribute(false, BMT_NODE_TYPE_RELAY);
}
bool bmt_ota_is_running(void)
{
	return atomic_load(&s_running);
}

/* Call once at boot (from main.c). Imports the HMAC key for the
 * OTA-beacon so it is ready before UART or RPC can trigger an OTA at
 * any moment. */
void bmt_ota_beacon_key_init(void)
{
	beacon_hmac_key_init();
}
#define BMT_OTA_GATEWAY_AUTO_CHECK_INTERVAL_MS (3 * 60 * 1000)

static void gateway_auto_check_task(void* arg)
{
	(void)arg;
	while (1)
	{
		vTaskDelay(pdMS_TO_TICKS(BMT_OTA_GATEWAY_AUTO_CHECK_INTERVAL_MS));
		bmt_ota_gateway_self_update();
	}
}

void bmt_ota_start_auto_check(void)
{
	xTaskCreate(gateway_auto_check_task, "bmt_ota_chk", 3072, NULL, 3, NULL);
}
