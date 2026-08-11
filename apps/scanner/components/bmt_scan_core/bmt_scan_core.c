#include "bmt_scan_core.h"

#include "esp_log.h"
#include "nvs.h"

#include "ble_mesh_example_init.h"

#include "bmt_auth.h"
#include "bmt_config.h"
#include "bmt_mesh.h"
#include "bmt_scan.h"
#include "bmt_uart.h"
#include "bmt_ota.h"

static const char* TAG = "BMT_CORE";

#define SCANNER_NVS_KEY_ID "scanner_id"

/* Dynamic scanner ID — loaded from NVS at boot, set via UART 'i'.
 * Defaults to 0x01 when NVS has no value (first flash). */
static uint8_t s_scanner_id = 0x01;

static void nvs_load_id(void)
{
	nvs_handle_t h;
	esp_err_t err = nvs_open(BMT_SCAN_NVS_NAMESPACE, NVS_READONLY, &h);
	if (err == ESP_ERR_NVS_NOT_FOUND)
	{
		ESP_LOGI(TAG, "[NVS] No scanner_id saved, using default: 0x%02X", s_scanner_id);
		return;
	}
	if (err != ESP_OK)
		return;
	uint8_t id = 0;
	if (nvs_get_u8(h, SCANNER_NVS_KEY_ID, &id) == ESP_OK && id >= 1 && id <= 8)
	{
		s_scanner_id = id;
		ESP_LOGI(TAG, "[NVS] Scanner ID loaded: 0x%02X", s_scanner_id);
	}
	else
	{
		ESP_LOGW(TAG, "[NVS] Invalid scanner_id in NVS, using default: 0x%02X", s_scanner_id);
	}
	nvs_close(h);
}

static void nvs_save_id(uint8_t id)
{
	nvs_handle_t h;
	if (nvs_open(BMT_SCAN_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
		return;
	nvs_set_u8(h, SCANNER_NVS_KEY_ID, id);
	nvs_commit(h);
	nvs_close(h);
	ESP_LOGI(TAG, "[NVS] Scanner ID saved: 0x%02X", id);
}

uint8_t bmt_scan_core_scanner_id(void)
{
	return s_scanner_id;
}

void bmt_scan_core_set_scanner_id(uint8_t id)
{
	nvs_save_id(id);
	s_scanner_id = id;
}

esp_err_t bmt_scan_core_init(void)
{
	nvs_load_id();
	ESP_LOGI(TAG, "Scanner ID: 0x%02X", s_scanner_id);

	/* [SECURITY] Import the HMAC key into the PSA Crypto keystore once
	 * at boot. MUST be called before bmt_scan starts receiving or
	 * verifying any Tag ADV. */
	bmt_auth_init();

	esp_err_t err = bluetooth_init();
	if (err != ESP_OK)
	{
		ESP_LOGE(TAG, "BT init failed!");
		return err;
	}

	err = bmt_mesh_init();
	if (err != ESP_OK)
	{
		ESP_LOGE(TAG, "Mesh init failed!");
		return err;
	}

	err = bmt_scan_start();
	if (err != ESP_OK)
	{
		ESP_LOGE(TAG, "Scan start failed!");
		return err;
	}

	bmt_uart_init();

	bmt_ota_init(&(bmt_ota_config_t){
	    .url = BMT_OTA_SCANNER_URL,
	    .server_cn = BMT_OTA_SERVER_CN,
	    .wifi_ssid = BMT_WIFI_SSID,
	    .wifi_pass = BMT_WIFI_PASS,
	    .nvs_namespace = BMT_SCAN_NVS_NAMESPACE,
	    .wifi_timeout_ms = BMT_OTA_WIFI_TIMEOUT_MS,
	    .silence_log_during_ota = true,
	});
	bmt_ota_start_pending_report_task();
	bmt_ota_start_auto_check();

	ESP_LOGI(TAG, "=== Scan Node READY (r=reset, i=set ID, 1=status) ===");
	printf("BMT Scanner Ready\n");
	return ESP_OK;
}
