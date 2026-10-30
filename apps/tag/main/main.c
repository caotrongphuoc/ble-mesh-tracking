
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "bmt_config.h"
#include "bmt_auth.h"
#include "bmt_beacon.h"
#include "bmt_uart.h"

static const char* TAG = "BMT_MAIN";

void app_main(void)
{
	ESP_LOGI(TAG, "=== BLE Tag v3.0-modular Starting ===");
	ESP_LOGI(TAG, "Battery=%u%%  Minor=0x%04X  TXPwrRef=%ddBm",
	         BMT_TAG_BATTERY, BMT_TAG_MINOR, BMT_TAG_TX_POWER);

	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
	    err == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);

	/* [SECURITY] Import the HMAC key into the PSA Crypto keystore
	 * once at boot. MUST be called before bmt_beacon_start()
	 * (build_adv_data needs the key ready). */
	bmt_auth_init();

	ESP_ERROR_CHECK(bmt_beacon_start());
	ESP_ERROR_CHECK(bmt_uart_init());

	ESP_LOGI(TAG, "=== Tag READY (1=status) ===");
}
