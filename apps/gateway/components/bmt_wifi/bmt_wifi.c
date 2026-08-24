#include "bmt_wifi.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "bmt_config.h"

static const char* TAG = "BMT_WIFI";
static const int WIFI_CONNECTED_BIT = BIT0;
#define BMT_WIFI_INIT_TIMEOUT_MS 15000

static EventGroupHandle_t s_evgrp;

static void event_handler(void* arg, esp_event_base_t base, int32_t id, void* data)
{
	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
	{
		esp_wifi_connect();
	}
	else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
	{
		ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
		esp_wifi_connect();
		xEventGroupClearBits(s_evgrp, WIFI_CONNECTED_BIT);
	}
	else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
	{
		ip_event_got_ip_t* ev = (ip_event_got_ip_t*)data;
		ESP_LOGI(TAG, "WiFi connected! IP: " IPSTR, IP2STR(&ev->ip_info.ip));
		xEventGroupSetBits(s_evgrp, WIFI_CONNECTED_BIT);
	}
}

void bmt_wifi_init(void)
{
	s_evgrp = xEventGroupCreate();
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	esp_event_handler_instance_t any_id, got_ip;
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &any_id));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &got_ip));
	wifi_config_t wifi_cfg = {.sta = {.threshold.authmode = WIFI_AUTH_WPA2_PSK}};
	strncpy((char*)wifi_cfg.sta.ssid, BMT_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
	strncpy((char*)wifi_cfg.sta.password, BMT_WIFI_PASS, sizeof(wifi_cfg.sta.password));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
	ESP_ERROR_CHECK(esp_wifi_start());
	EventBits_t bits = xEventGroupWaitBits(s_evgrp, WIFI_CONNECTED_BIT,
	                                       pdFALSE, pdFALSE,
	                                       pdMS_TO_TICKS(BMT_WIFI_INIT_TIMEOUT_MS));
	if (!(bits & WIFI_CONNECTED_BIT))
	{
		ESP_LOGW(TAG, "WiFi connect timeout %ds - boot continues, MQTT will retry when WiFi comes up",
		         BMT_WIFI_INIT_TIMEOUT_MS / 1000);
	}
}
