#include "bmt_scan_core.h"

#include "esp_log.h"

#include "ble_mesh_example_init.h"

#include "bmt_auth.h"
#include "bmt_config.h"
#include "bmt_mesh.h"
#include "bmt_scan.h"
#include "bmt_scanner_id.h"
#include "bmt_uart.h"
#include "bmt_ota.h"

static const char* TAG = "BMT_CORE";

esp_err_t bmt_scan_core_init(void)
{
	bmt_scanner_id_load();
	ESP_LOGI(TAG, "Scanner ID: 0x%02X", bmt_scanner_id());

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
