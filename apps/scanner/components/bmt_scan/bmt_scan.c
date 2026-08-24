#include <assert.h>
#include "bmt_scan.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_gap_ble_api.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "bmt_types.h"
#include "bmt_auth.h"
#include "bmt_tag_table.h"
#include "bmt_mesh.h"
#include "bmt_ota.h"
#include "bmt_scan_core.h"

static const char* TAG = "BMT_SCAN";
#define BMT_OTA_BEACON_COOLDOWN_US (20 * 1000 * 1000)
static uint16_t s_last_ota_beacon_mac = 0;
static int64_t s_last_ota_beacon_us = -((int64_t)BMT_OTA_BEACON_COOLDOWN_US);
#define BMT_GAP_SCAN_DURATION_MS 800
#define BMT_MESH_PUBLISH_DURATION_MS 700
/* Scan window < interval -> leave a gap for the mesh bearer to RX ANNOUNCE
 * window=0x30 (30ms), interval=0x50 (50ms) → 60% duty, 40% for mesh */
#define BMT_SCAN_INTERVAL_UNITS 0x0050
#define BMT_SCAN_WINDOW_UNITS 0x0030

typedef enum
{
	PHASE_GAP_SCAN = 0,
	PHASE_MESH_PUB = 1
} radio_phase_t;
static volatile radio_phase_t s_phase = PHASE_GAP_SCAN;
static volatile bool s_has_new_data = false;

static esp_ble_scan_params_t s_scan_params = {
    .scan_type = BLE_SCAN_TYPE_PASSIVE,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = BMT_SCAN_INTERVAL_UNITS,
    .scan_window = BMT_SCAN_WINDOW_UNITS,
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
};
static const uint8_t BMT_SYSTEM_UUID_PREFIX[4] = {0xAB, 0x00, 0x00, 0x00};

static bool parse_tag_payload(uint8_t* adv_data, uint8_t adv_len, bmt_tag_payload_t* out)
{
	if (!adv_data || adv_len < 4 || !out)
		return false;
	int pos = 0;
	while (pos < adv_len)
	{
		uint8_t field_len = adv_data[pos];
		if (field_len == 0 || pos + field_len >= adv_len)
			break;
		uint8_t field_type = adv_data[pos + 1];
		if (field_type == 0xFF && field_len >= 3)
		{
			uint16_t cid = (uint16_t)adv_data[pos + 2] | ((uint16_t)adv_data[pos + 3] << 8);
			if (cid == BMT_CID_ESPRESSIF && field_len >= (1 + 2 + 24))
			{
				/* Copy into an aligned buffer - adv_data + pos + 4 may sit
				 * at an odd address; reading uint16_t directly from there
				 * faults on Xtensa / RISC-V. */
				bmt_tag_adv_payload_t p;
				memcpy(&p, adv_data + pos + 4, sizeof(p));
				if (memcmp(p.uuid, BMT_SYSTEM_UUID_PREFIX, 4) != 0)
					goto next_field;
				if (!bmt_auth_verify_tag(p.minor, (uint8_t*)&p,
				                         sizeof(p) - sizeof(p.mac16), p.mac16))
				{
					ESP_LOGW(TAG, "[AUTH] reject tag_id=0x%04x seq=%u mac_rx=0x%04x",
					         p.minor, p.sequence, p.mac16);
					goto next_field;
				}
				/* This field used to hold major (PERSON/ASSET); it now holds battery % 0-100 */
				out->battery = (uint8_t)p.battery;
				out->tag_id = p.minor;
				out->tx_power = p.tx_power;
				out->sequence = p.sequence;
				out->mac16 = p.mac16;
				return true;
			}
			if (cid == BMT_CID_APPLE && field_len >= 26 && adv_data[pos + 4] == 0x02 && adv_data[pos + 5] == 0x15)
			{
				uint16_t major = ((uint16_t)adv_data[pos + 22] << 8) | adv_data[pos + 23];
				uint16_t minor = ((uint16_t)adv_data[pos + 24] << 8) | adv_data[pos + 25];
				if (major != BMT_TAG_MAJOR_PERSON && major != BMT_TAG_MAJOR_ASSET)
					goto next_field;
				/* iPhone-as-tag does not report a system battery % -> 0 (unknown) */
				out->battery = 0;
				out->tag_id = minor | 0x8000;
				out->tx_power = BMT_PHONE_TX_POWER_1M;
				out->sequence = 0;
				out->mac16 = 0;
				return true;
			}
		}
	next_field:
		pos += field_len + 1;
	}
	return false;
}
static void gap_event_handler(esp_gap_ble_cb_event_t event,
                              esp_ble_gap_cb_param_t* param)
{
	switch (event)
	{
	case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
		ESP_LOGI(TAG, "Scan params set OK");
		break;
	case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
		if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS)
			ESP_LOGE(TAG, "Scan start FAILED");
		break;
	case ESP_GAP_BLE_SCAN_RESULT_EVT:
	{
		if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT)
			break;

		if (!bmt_ota_is_triggered())
		{
			uint8_t* adv = param->scan_rst.ble_adv;
			uint8_t len = param->scan_rst.adv_data_len;
			for (int i = 0; i + 10 < len;)
			{
				uint8_t field_len = adv[i];
				if (field_len == 0)
					break;
				uint8_t field_type = adv[i + 1];
				if (field_type == 0xFF && field_len >= 10 &&
				    adv[i + 2] == 0xE5 && adv[i + 3] == 0x02 &&
				    adv[i + 4] == 'B' && adv[i + 5] == 'M' &&
				    adv[i + 6] == 'T' && adv[i + 7] == 0xFA &&
				    (adv[i + 8] == BMT_NODE_TYPE || adv[i + 8] == 0xFF))
				{

					uint16_t recv_mac = ((uint16_t)adv[i + 9] << 8) | adv[i + 10];
					uint16_t calc_mac = bmt_auth_ota_beacon_hmac16(&adv[i + 4], 5);

					if (recv_mac != calc_mac)
					{
						ESP_LOGW(TAG, "[OTA] Beacon HMAC mismatch (got 0x%04x, expect 0x%04x)"
						              " - ignoring, possibly a forged beacon",
						         recv_mac, calc_mac);
						break;
					}

					int64_t now = esp_timer_get_time();
					if (recv_mac == s_last_ota_beacon_mac &&
					    (now - s_last_ota_beacon_us) < BMT_OTA_BEACON_COOLDOWN_US)
					{
						break;
					}
					s_last_ota_beacon_mac = recv_mac;
					s_last_ota_beacon_us = now;

					printf("[OTA] BLE beacon from Gateway (HMAC OK) - triggering WiFi OTA!\n");
					bmt_ota_trigger();
					break;
				}
				i += field_len + 1;
			}
		}
		if (bmt_ota_is_triggered())
			break;
		if (s_phase != PHASE_GAP_SCAN)
			break;
		EventBits_t bits = xEventGroupGetBits(bmt_mesh_evgrp());
		if (!(bits & BMT_PROV_COMPLETE_BIT))
			break;

		bmt_tag_payload_t payload;
		if (!parse_tag_payload(param->scan_rst.ble_adv,
		                       param->scan_rst.adv_data_len, &payload))
			break;

		int idx = bmt_tag_table_find(payload.tag_id);
		if (idx < 0)
			idx = bmt_tag_table_add(payload.tag_id, payload.battery,
			                        payload.tx_power, param->scan_rst.rssi,
			                        payload.sequence, param->scan_rst.bda);
		else
			bmt_tag_table_update(idx, param->scan_rst.rssi, payload.sequence,
			                     payload.battery);

		if (idx >= 0)
			bmt_tag_table_set_epoch(idx, bmt_auth_get_locked_epoch(payload.tag_id));

		s_has_new_data = true;
		break;
	}
	default:
		break;
	}
}
static void radio_manager_task(void* arg)
{
	(void)arg;
	ESP_LOGI(TAG, "Waiting for provision...");
	xEventGroupWaitBits(bmt_mesh_evgrp(), BMT_PROV_COMPLETE_BIT,
	                    pdFALSE, pdTRUE, portMAX_DELAY);
	vTaskDelay(pdMS_TO_TICKS(3000));
	esp_ble_gap_stop_scanning();
	vTaskDelay(pdMS_TO_TICKS(200));

	esp_err_t err = esp_ble_gap_set_scan_params(&s_scan_params);
	if (err != ESP_OK)
	{
		ESP_LOGE(TAG, "set_scan_params failed");
		vTaskDelete(NULL);
		return;
	}

	ESP_LOGI(TAG, "=== Radio Manager started ===");
	s_phase = PHASE_MESH_PUB;

	while (1)
	{
		/* OTA active: stop GAP scan so WiFi OTA gets the whole radio. */
		if (bmt_ota_is_triggered())
		{
			if (s_phase == PHASE_GAP_SCAN)
			{
				esp_ble_gap_stop_scanning();
				s_phase = PHASE_MESH_PUB;
				ESP_LOGI(TAG, "[OTA] GAP scan paused for WiFi OTA");
			}
			vTaskDelay(pdMS_TO_TICKS(500));
			continue;
		}

		s_phase = PHASE_GAP_SCAN;
		s_has_new_data = false;
		esp_ble_gap_start_scanning(0);
		vTaskDelay(pdMS_TO_TICKS(BMT_GAP_SCAN_DURATION_MS));

		s_phase = PHASE_MESH_PUB;
		esp_ble_gap_stop_scanning();
		vTaskDelay(pdMS_TO_TICKS(100));

		if (s_has_new_data)
		{
			bmt_mesh_publish_tags();
			bmt_tag_table_print(bmt_scan_core_scanner_id());
			s_has_new_data = false;
		}
		vTaskDelay(pdMS_TO_TICKS(BMT_MESH_PUBLISH_DURATION_MS - 100));
	}
}

static void timeout_check_task(void* arg)
{
	(void)arg;
	while (1)
	{
		if (!bmt_ota_is_triggered())
		{
			bmt_tag_table_check_timeouts();
		}
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}

esp_err_t bmt_scan_start(void)
{
	esp_err_t err = esp_ble_gap_register_callback(gap_event_handler);
	if (err != ESP_OK)
		return err;

	assert(xTaskCreate(radio_manager_task, "bmt_radio", 3072, NULL, 6, NULL) == pdPASS);
	assert(xTaskCreate(timeout_check_task, "bmt_timeout", 2048, NULL, 3, NULL) == pdPASS);
	return ESP_OK;
}
