
#include <assert.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_mesh_example_init.h"

#include "bmt_config.h"
#include "bmt_factory_reset.h"
#include "bmt_mesh.h"
#include "bmt_mqtt.h"
#include "bmt_node_table.h"
#include "bmt_ota.h"
#include "bmt_thingsboard.h"
#include "bmt_uart.h"
#include "bmt_watchdog.h"
#include "bmt_wifi.h"
#include "bmt_zone.h"

static const char* TAG = "BMT_GW";

#define BMT_WDT_TIMEOUT_S 60

static void wdt_feed_task(void* arg)
{
	(void)arg;
	esp_task_wdt_add(NULL);
	while (1)
	{
		esp_task_wdt_reset();
		vTaskDelay(pdMS_TO_TICKS(10000));
	}
}

void app_main(void)
{
	esp_err_t err;
	ESP_LOGI(TAG, "=== BMT Gateway v5.0 Starting ===");

	err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_LOGE(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
		ESP_LOGE(TAG, "!!! NVS init: %s - MUST WIPE THE ENTIRE NVS", esp_err_to_name(err));
		ESP_LOGE(TAG, "!!! => LOSES node table + NetKey/AppKey (new keys will be generated)");
		ESP_LOGE(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);

	bmt_factory_reset_init();

	bmt_node_table_load();
	bmt_zone_init();

	/* Generate random NetKey / AppKey if the mesh NVS is empty. MUST
	 * be called before bmt_mesh_init(), which uses these keys to add
	 * them to the mesh stack. */
	bmt_mesh_generate_keys_if_needed();

	/* [SECURITY] Import the OTA-beacon HMAC key early - OTA can be
	 * triggered at any moment via UART ('u') or RPC from ThingsBoard. */
	bmt_ota_beacon_key_init();
	bmt_ota_start_key_rotation();

	bmt_mqtt_start_worker();

	bmt_wifi_init();
	bmt_mqtt_init();
	vTaskDelay(pdMS_TO_TICKS(2000));

	err = bluetooth_init();
	if (err != ESP_OK)
	{
		ESP_LOGE(TAG, "BT init failed");
		return;
	}

	err = bmt_mesh_init();
	if (err != ESP_OK)
	{
		ESP_LOGE(TAG, "Mesh init failed");
		return;
	}

	printf("\n================ BMT GATEWAY v5.0-modular-nimble ================\n");
	bmt_mesh_print_keys();
	printf("TB     : %s\n", BMT_TB_HOST);
	printf("Device : %s\n", BMT_DEV_NAME_GATEWAY);
	printf("\nZONE MAPPING:\n");
	printf("  scanner 0x01 -> %s\n", bmt_zone_name(0x01));
	printf("  scanner 0x02 -> %s\n", bmt_zone_name(0x02));
	printf("  scanner 0x03 -> %s\n", bmt_zone_name(0x03));
	printf("\nOTA CONFIG:\n");
	printf("  Scanner : %s\n", BMT_OTA_SCANNER_URL);
	printf("  Relay   : %s\n", BMT_OTA_RELAY_URL);
	printf("  Gateway : %s\n", BMT_OTA_GATEWAY_URL);
	printf("===================================================\n");

	bmt_node_table_print();

	bmt_uart_start();

	esp_task_wdt_config_t wdt_cfg = {
	    .timeout_ms = BMT_WDT_TIMEOUT_S * 1000,
	    .idle_core_mask = 0,
	    .trigger_panic = true,
	};
	esp_task_wdt_reconfigure(&wdt_cfg);

	bmt_mesh_start_node_ping();
	bmt_tb_start_zone_timeout_task();
	bmt_watchdog_start();
	bmt_ota_start_auto_check();
	assert(xTaskCreate(wdt_feed_task, "bmt_wdt_feed", 2048, NULL, 2, NULL) == pdPASS);

	ESP_LOGI(TAG, "=== BMT Gateway v5.0-modular-nimble READY ===");
	printf("Gateway Ready\n");
}
