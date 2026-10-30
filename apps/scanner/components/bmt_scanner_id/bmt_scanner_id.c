#include "bmt_scanner_id.h"

#include "esp_log.h"
#include "nvs.h"

#include "bmt_config.h"

static const char* TAG = "BMT_CORE";

#define SCANNER_NVS_KEY_ID "scanner_id"

static uint8_t s_scanner_id = 0x01;

void bmt_scanner_id_load(void)
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

uint8_t bmt_scanner_id(void)
{
	return s_scanner_id;
}

void bmt_scanner_id_set(uint8_t id)
{
	nvs_save_id(id);
	s_scanner_id = id;
}
