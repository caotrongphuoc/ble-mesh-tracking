#include <stdlib.h>
#include "bmt_ota.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "bmt_https_ota.h"
#include "bmt_mesh_node.h" /* bmt_mesh_report_ota_result */

static const char* TAG = "BMT_OTA";

#define BMT_OTA_NVS_KEY_PENDING "ota_pending"
#define BMT_OTA_DEFAULT_WIFI_TIMEOUT_MS 30000
#define BMT_OTA_DEFAULT_AUTO_CHECK_MS (3 * 60 * 1000)

static bmt_ota_config_t s_cfg;
static bool s_inited = false;

static volatile bool s_ota_triggered = false;
static EventGroupHandle_t s_wifi_evgrp = NULL;
static const int WIFI_CONNECTED_BIT = BIT0;
static esp_netif_t* s_sta_netif = NULL;
static esp_event_handler_instance_t s_evt_any_id = NULL;
static esp_event_handler_instance_t s_evt_got_ip = NULL;

void bmt_ota_init(const bmt_ota_config_t* cfg)
{
	if (!cfg || !cfg->url || !cfg->server_cn || !cfg->wifi_ssid || !cfg->wifi_pass || !cfg->nvs_namespace)
	{
		ESP_LOGE(TAG, "bmt_ota_init: config is missing a required field");
		return;
	}
	s_cfg = *cfg;
	if (s_cfg.wifi_timeout_ms == 0)
		s_cfg.wifi_timeout_ms = BMT_OTA_DEFAULT_WIFI_TIMEOUT_MS;
	if (s_cfg.auto_check_interval_ms == 0)
		s_cfg.auto_check_interval_ms = BMT_OTA_DEFAULT_AUTO_CHECK_MS;
	s_inited = true;
	ESP_LOGI(TAG, "OTA config: url=%s nvs_ns=%s wifi_timeout=%" PRIu32 "ms",
	         s_cfg.url, s_cfg.nvs_namespace, s_cfg.wifi_timeout_ms);
}

bool bmt_ota_is_triggered(void)
{
	return s_ota_triggered;
}

static void mark_pending(void)
{
	nvs_handle_t h;
	esp_err_t err = nvs_open(s_cfg.nvs_namespace, NVS_READWRITE, &h);
	if (err != ESP_OK)
	{
		ESP_LOGE(TAG, "mark_pending: nvs_open fail: %s", esp_err_to_name(err));
		return;
	}
	err = nvs_set_u8(h, BMT_OTA_NVS_KEY_PENDING, 1);
	if (err != ESP_OK)
		ESP_LOGE(TAG, "mark_pending: nvs_set_u8 fail: %s", esp_err_to_name(err));
	else if ((err = nvs_commit(h)) != ESP_OK)
		ESP_LOGE(TAG, "mark_pending: nvs_commit fail: %s", esp_err_to_name(err));
	nvs_close(h);
}

static bool check_and_clear_pending(void)
{
	nvs_handle_t h;
	uint8_t val = 0;
	if (nvs_open(s_cfg.nvs_namespace, NVS_READWRITE, &h) != ESP_OK)
		return false;
	nvs_get_u8(h, BMT_OTA_NVS_KEY_PENDING, &val);
	if (val)
	{
		nvs_set_u8(h, BMT_OTA_NVS_KEY_PENDING, 0);
		nvs_commit(h);
	}
	nvs_close(h);
	return val != 0;
}

static void wifi_event_handler(void* arg, esp_event_base_t base,
                               int32_t id, void* data)
{
	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
	{
		esp_wifi_connect();
	}
	else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
	{
		esp_wifi_connect();
		xEventGroupClearBits(s_wifi_evgrp, WIFI_CONNECTED_BIT);
	}
	else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
	{
		ip_event_got_ip_t* ev = (ip_event_got_ip_t*)data;
		ESP_LOGI(TAG, "[OTA] WiFi got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
		xEventGroupSetBits(s_wifi_evgrp, WIFI_CONNECTED_BIT);
	}
}

static esp_err_t ota_wifi_bring_up(void)
{
	s_wifi_evgrp = xEventGroupCreate();
	esp_err_t err = esp_netif_init();
	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
	{
		printf("[OTA] netif init failed: %s\n", esp_err_to_name(err));
		return err;
	}
	esp_event_loop_create_default();
	s_sta_netif = esp_netif_create_default_wifi_sta();

	wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
	err = esp_wifi_init(&wcfg);
	if (err != ESP_OK)
	{
		printf("[OTA] wifi init failed: %s\n", esp_err_to_name(err));
		return err;
	}
	esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &s_evt_any_id);
	esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &s_evt_got_ip);

	wifi_config_t wifi_cfg = {.sta = {.threshold.authmode = WIFI_AUTH_WPA2_PSK}};
	strncpy((char*)wifi_cfg.sta.ssid, s_cfg.wifi_ssid, sizeof(wifi_cfg.sta.ssid));
	strncpy((char*)wifi_cfg.sta.password, s_cfg.wifi_pass, sizeof(wifi_cfg.sta.password));
	esp_wifi_set_mode(WIFI_MODE_STA);
	esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
	esp_wifi_start();

	printf("[OTA] Connecting WiFi (max %" PRIu32 "s)...\n", s_cfg.wifi_timeout_ms / 1000);
	EventBits_t bits = xEventGroupWaitBits(s_wifi_evgrp, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(s_cfg.wifi_timeout_ms));
	if (!(bits & WIFI_CONNECTED_BIT))
	{
		printf("[OTA] WiFi connect timeout\n");
		return ESP_ERR_TIMEOUT;
	}
	return ESP_OK;
}

static void ota_wifi_tear_down(void)
{
	esp_wifi_stop();
	if (s_evt_any_id)
	{
		esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_evt_any_id);
		s_evt_any_id = NULL;
	}
	if (s_evt_got_ip)
	{
		esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_evt_got_ip);
		s_evt_got_ip = NULL;
	}
	esp_wifi_deinit();
	if (s_sta_netif)
	{
		esp_netif_destroy_default_wifi(s_sta_netif);
		s_sta_netif = NULL;
	}
	if (s_wifi_evgrp)
	{
		vEventGroupDelete(s_wifi_evgrp);
		s_wifi_evgrp = NULL;
	}
}

static void ota_wifi_task(void* arg)
{
	(void)arg;

	if (s_cfg.silence_log_during_ota)
		esp_log_level_set("*", ESP_LOG_NONE);

	printf("\n[OTA] ===== WiFi OTA triggered =====\n");
	printf("[OTA] URL: %s\n", s_cfg.url);

	bool do_report_fail = true;
	esp_err_t err = ota_wifi_bring_up();
	if (err == ESP_OK)
	{
		printf("[OTA] WiFi connected - checking firmware version...\n");
		bmt_https_ota_cfg_t fetch = {
		    .url = s_cfg.url,
		    .server_cn = s_cfg.server_cn,
		    .timeout_ms = 120000,
		    .keep_alive = true,
		};
		err = bmt_https_ota_fetch_and_flash(&fetch);
		if (err == ESP_OK)
		{
			printf("[OTA] ===== OTA SUCCESS - rebooting =====\n");
			mark_pending();
			vTaskDelay(pdMS_TO_TICKS(1000));
			esp_restart();
		}
		else if (err == ESP_ERR_NOT_FOUND)
		{
			do_report_fail = false;
		}
	}

	ota_wifi_tear_down();

	if (s_cfg.silence_log_during_ota)
		esp_log_level_set("*", ESP_LOG_INFO);

	if (do_report_fail)
	{
		bmt_mesh_report_ota_result(1);
		printf("[OTA] failed - back to BLE-only\n");
	}
	else
	{
		printf("[OTA] No update needed - back to BLE-only\n");
	}
	s_ota_triggered = false;
	vTaskDelete(NULL);
}

void bmt_ota_trigger(void)
{
	if (!s_inited)
	{
		ESP_LOGE(TAG, "bmt_ota_trigger called before bmt_ota_init - ignored");
		return;
	}
	if (s_ota_triggered)
	{
		ESP_LOGW(TAG, "Already triggered");
		return;
	}
	s_ota_triggered = true;
	if (xTaskCreate(ota_wifi_task, "bmt_ota_wifi", 8192, NULL, 5, NULL) != pdPASS)
		abort();
}

static void report_pending_task(void* arg)
{
	(void)arg;
	vTaskDelay(pdMS_TO_TICKS(5000));
	if (check_and_clear_pending())
	{
		ESP_LOGI(TAG, "Detected a fresh OTA + successful reboot - reporting back to Gateway");
		bmt_mesh_report_ota_result(0);
	}
	vTaskDelete(NULL);
}

void bmt_ota_start_pending_report_task(void)
{
	if (!s_inited)
	{
		ESP_LOGE(TAG, "start_pending_report called before bmt_ota_init - ignored");
		return;
	}
	if (xTaskCreate(report_pending_task, "ota_rpt", 2048, NULL, 3, NULL) != pdPASS)
		abort();
}

static void auto_check_task(void* arg)
{
	(void)arg;
	while (1)
	{
		vTaskDelay(pdMS_TO_TICKS(s_cfg.auto_check_interval_ms));
		bmt_ota_trigger();
	}
}

void bmt_ota_start_auto_check(void)
{
	if (!s_inited)
	{
		ESP_LOGE(TAG, "start_auto_check called before bmt_ota_init - ignored");
		return;
	}
	if (xTaskCreate(auto_check_task, "bmt_ota_chk", 2048, NULL, 3, NULL) != pdPASS)
		abort();
}
