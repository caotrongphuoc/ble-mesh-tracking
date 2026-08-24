#include <assert.h>
#include "bmt_mqtt.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "bmt_config.h"
#include "bmt_ota.h"
#include "bmt_thingsboard.h"

static const char* TAG = "BMT_MQTT";

#define BMT_MQTT_QUEUE_SIZE 64
#define BMT_MQTT_PUBLISH_TIMEOUT_MS 500
extern const uint8_t bmt_ca_pem_start[] asm("_binary_ca_pem_start");
extern const uint8_t bmt_ca_pem_end[] asm("_binary_ca_pem_end");

static esp_mqtt_client_handle_t s_client = NULL;
static bool s_connected = false;
typedef struct
{
	bmt_tag_report_t report;
	uint8_t scanner_mac[6];
	bool has_mac;
} bmt_tag_report_item_t;

static QueueHandle_t s_queue = NULL;
static uint32_t s_enqueued = 0;
static uint32_t s_dropped = 0;
static uint32_t s_published = 0;

bool bmt_mqtt_is_connected(void)
{
	return s_connected;
}
esp_mqtt_client_handle_t bmt_mqtt_get_client(void)
{
	return s_client;
}
void bmt_mqtt_note_published(void)
{
	s_published++;
}

void bmt_mqtt_enqueue_tag_report(const bmt_tag_report_t* report, const uint8_t* scanner_mac)
{
	if (!s_queue)
		return;
	bmt_tag_report_item_t item;
	item.report = *report;
	if (scanner_mac)
	{
		memcpy(item.scanner_mac, scanner_mac, 6);
		item.has_mac = true;
	}
	else
	{
		memset(item.scanner_mac, 0, 6);
		item.has_mac = false;
	}

	if (xQueueSend(s_queue, &item, 0) == pdTRUE)
	{
		s_enqueued++;
	}
	else
	{
		s_dropped++;
		ESP_LOGW(TAG, "MQTT queue FULL (dropped: %" PRIu32 ")", s_dropped);
	}
}
void bmt_mqtt_log_stats(void)
{
	UBaseType_t in_q = s_queue ? uxQueueMessagesWaiting(s_queue) : 0;
	printf("\n========== MQTT / MESH STATS ==========\n");
	printf("MQTT Connected  : %s\n", s_connected ? "YES" : "NO");
	printf("Queue size      : %u / %d\n", (unsigned)in_q, BMT_MQTT_QUEUE_SIZE);
	printf("Total enqueued  : %" PRIu32 "\n", s_enqueued);
	printf("Total published : %" PRIu32 "  <- MQTT layer\n", s_published);
	printf("Total dropped   : %" PRIu32 "\n", s_dropped);
	if (s_enqueued > 0)
		printf("Drop rate       : %.2f%%\n", (float)s_dropped * 100.0f / s_enqueued);
	printf("=======================================\n");
}

static void mqtt_worker_task(void* arg)
{
	(void)arg;
	bmt_tag_report_item_t item;
	ESP_LOGI(TAG, "MQTT worker task started");
	while (1)
	{
		if (xQueueReceive(s_queue, &item, portMAX_DELAY) == pdTRUE)
			bmt_tb_pub_tag_report(&item.report, item.has_mac ? item.scanner_mac : NULL);
	}
}

void bmt_mqtt_start_worker(void)
{
	s_queue = xQueueCreate(BMT_MQTT_QUEUE_SIZE, sizeof(bmt_tag_report_item_t));
	if (!s_queue)
	{
		ESP_LOGE(TAG, "MQTT queue create failed");
		return;
	}
	assert(xTaskCreate(mqtt_worker_task, "bmt_mqtt_wkr", 4096, NULL, 5, NULL) == pdPASS);
}

static void mqtt_event_handler(void* args, esp_event_base_t base, int32_t id, void* data)
{
	switch ((esp_mqtt_event_id_t)id)
	{
	case MQTT_EVENT_CONNECTED:
		s_connected = true;
		ESP_LOGI(TAG, "MQTT connected to ThingsBoard");
		esp_mqtt_client_subscribe(s_client, "v1/devices/me/rpc/request/+", 1);
		ESP_LOGI(TAG, "Subscribed to RPC topic");
		bmt_tb_pub_gateway_online();
		bmt_tb_reconnect_all_devices();
		break;

	case MQTT_EVENT_DISCONNECTED:
		s_connected = false;
		ESP_LOGW(TAG, "MQTT disconnected");
		break;

	case MQTT_EVENT_DATA:
	{
		esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
		if (!ev->topic || !ev->data)
			break;
		/* ev->topic is NOT guaranteed to be null-terminated - check
		 * topic_len first so strncmp does not read past the buffer. */
		if (ev->topic_len < 26)
			break;
		if (strncmp(ev->topic, "v1/devices/me/rpc/request/", 26) != 0)
			break;

		char payload[128] = {0};
		int plen = ev->data_len < (int)sizeof(payload) - 1 ? ev->data_len : (int)sizeof(payload) - 1;
		memcpy(payload, ev->data, plen);
		ESP_LOGI(TAG, "[RPC] Received: %s", payload);

		if (strstr(payload, "ota_scanner"))
		{
			if (bmt_ota_is_running())
				ESP_LOGW(TAG, "[RPC] OTA already running");
			else
			{
				ESP_LOGI(TAG, "[RPC] OTA Scanner triggered");
				bmt_ota_trigger_all_scanners();
			}
		}
		else if (strstr(payload, "ota_relay"))
		{
			if (bmt_ota_is_running())
				ESP_LOGW(TAG, "[RPC] OTA already running");
			else
			{
				ESP_LOGI(TAG, "[RPC] OTA Relay triggered");
				bmt_ota_trigger_all_relays();
			}
		}
		else if (strstr(payload, "ota_gateway"))
		{
			ESP_LOGI(TAG, "[RPC] OTA Gateway triggered");
			bmt_ota_gateway_self_update();
		}
		else
		{
			ESP_LOGW(TAG, "[RPC] Unknown method: %s", payload);
		}
		break;
	}
	default:
		break;
	}
}

void bmt_mqtt_init(void)
{
	esp_mqtt_client_config_t cfg = {
	    .broker.address.uri = BMT_TB_HOST,
	    .broker.verification.certificate = (const char*)bmt_ca_pem_start,
	    .broker.verification.certificate_len = (size_t)(bmt_ca_pem_end - bmt_ca_pem_start),
	    .broker.verification.common_name = BMT_TB_CN,
	    .credentials.username = BMT_TB_GATEWAY_TOKEN,
	    .network.timeout_ms = BMT_MQTT_PUBLISH_TIMEOUT_MS,
	    .network.reconnect_timeout_ms = 5000,
	};
	s_client = esp_mqtt_client_init(&cfg);
	if (!s_client)
	{
		ESP_LOGE(TAG, "MQTT init failed");
		return;
	}
	esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
	esp_mqtt_client_start(s_client);
	ESP_LOGI(TAG, "MQTTS -> %s (verify CN=%s)", BMT_TB_HOST, BMT_TB_CN);
}
