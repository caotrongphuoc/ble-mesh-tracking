#include <assert.h>
#include "bmt_thingsboard.h"

#include <stdio.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bmt_config.h"
#include "bmt_mqtt.h"
#include "bmt_node_table.h"
#include "bmt_zone.h"
static void tb_connect_device(const char* dev, const char* profile)
{
	if (!bmt_mqtt_is_connected() || !bmt_mqtt_get_client())
		return;
	char json[96];
	snprintf(json, sizeof(json), "{\"device\":\"%s\",\"type\":\"%s\"}", dev, profile);
	esp_mqtt_client_publish(bmt_mqtt_get_client(), "v1/gateway/connect", json, 0, 1, 0);
}

static void tb_disconnect_device(const char* dev)
{
	if (!bmt_mqtt_is_connected() || !bmt_mqtt_get_client())
		return;
	char json[64];
	snprintf(json, sizeof(json), "{\"device\":\"%s\"}", dev);
	esp_mqtt_client_publish(bmt_mqtt_get_client(), "v1/gateway/disconnect", json, 0, 1, 0);
}

static void tb_set_role(const char* dev, const char* role)
{
	if (!bmt_mqtt_is_connected() || !bmt_mqtt_get_client())
		return;
	char json[128];
	snprintf(json, sizeof(json), "{\"%s\":{\"role\":\"%s\"}}", dev, role);
	esp_mqtt_client_publish(bmt_mqtt_get_client(), "v1/gateway/attributes", json, 0, 1, 0);
}

static bool tb_publish_tag_out_of_range(uint16_t tag_id)
{
	if (!bmt_mqtt_is_connected() || !bmt_mqtt_get_client())
		return false;

	char dev[32], json[192];
	snprintf(dev, sizeof(dev), BMT_DEV_NAME_TAG_FMT, tag_id);
	snprintf(json, sizeof(json),
	         "{\"%s\":{\"current_zone\":\"out_of_range\",\"current_zone_num\":0}}",
	         dev);
	return esp_mqtt_client_publish(bmt_mqtt_get_client(), "v1/gateway/attributes",
	                               json, 0, 1, 0) >= 0;
}

void bmt_tb_reconnect_all_devices(void)
{
	int reannounced = 0;

	for (int i = 0; i < bmt_node_table_capacity(); i++)
	{
		bmt_node_t* n = bmt_node_table_get(i);
		if (!n || !n->used)
			continue;
		char dev[32];
		snprintf(dev, sizeof(dev), BMT_DEV_NAME_NODE_FMT,
		         n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5]);
		tb_connect_device(dev, BMT_PROFILE_NODE);
		tb_set_role(dev, n->is_scan ? BMT_ROLE_SCAN : BMT_ROLE_RELAY);
		reannounced++;
	}

	bmt_zone_lock();
	for (int i = 0; i < bmt_zone_track_capacity(); i++)
	{
		bmt_tag_track_t* t = bmt_zone_track_get(i);
		if (!t || !t->active)
			continue;
		char dev[32];
		snprintf(dev, sizeof(dev), BMT_DEV_NAME_TAG_FMT, t->tag_id);
		tb_connect_device(dev, BMT_PROFILE_TAG);
		tb_set_role(dev, BMT_ROLE_TAG);
		if (t->out_of_range_pending && tb_publish_tag_out_of_range(t->tag_id))
		{
			t->out_of_range_pending = false;
			ESP_LOGI("BMT_TB", "Replayed pending out_of_range for tag 0x%04x", t->tag_id);
		}
		reannounced++;
	}
	bmt_zone_unlock();

	ESP_LOGI("BMT_TB", "Re-announced %d known device(s) to ThingsBoard after MQTT (re)connect", reannounced);
}

void bmt_tb_pub_gateway_online(void)
{
	if (!bmt_mqtt_is_connected() || !bmt_mqtt_get_client())
		return;
	esp_mqtt_client_publish(bmt_mqtt_get_client(), "v1/devices/me/telemetry",
	                        "{\"status\":\"ONLINE\"}", 0, 1, 0);
	esp_mqtt_client_publish(bmt_mqtt_get_client(), "v1/devices/me/attributes",
	                        "{\"role\":\"" BMT_ROLE_GATEWAY "\"}", 0, 1, 0);
	ESP_LOGI("BMT_TB", "TB Gateway ONLINE");
}

void bmt_tb_pub_gateway_ota_result(bool success)
{
	if (!bmt_mqtt_is_connected() || !bmt_mqtt_get_client())
		return;
	const char* json = success ? "{\"ota_result\":\"SUCCESS\"}" : "{\"ota_result\":\"FAILED\"}";
	esp_mqtt_client_publish(bmt_mqtt_get_client(), "v1/devices/me/telemetry", json, 0, 1, 0);
	ESP_LOGI("BMT_TB", "TB Gateway OTA result: %s", success ? "SUCCESS" : "FAILED");
}

void bmt_tb_pub_node_status(uint16_t addr, const uint8_t* mac, const char* role, bool online)
{
	if (!bmt_mqtt_is_connected() || !bmt_mqtt_get_client())
		return;
	char dev[32], json[192];
	if (mac)
		snprintf(dev, sizeof(dev), BMT_DEV_NAME_NODE_FMT,
		         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	else
		snprintf(dev, sizeof(dev), "bmt_node_addr%04x", addr); /* rare fallback: MAC not resolved yet */
	if (online)
	{
		tb_connect_device(dev, BMT_PROFILE_NODE);
		tb_set_role(dev, role);
	}
	snprintf(json, sizeof(json), "{\"%s\":[{\"status\":\"%s\",\"addr\":\"0x%04x\"}]}",
	         dev, online ? "ONLINE" : "OFFLINE", addr);
	esp_mqtt_client_publish(bmt_mqtt_get_client(), "v1/gateway/telemetry", json, 0, 1, 0);
	if (!online)
		tb_disconnect_device(dev);
}

void bmt_tb_pub_tag_report(const bmt_tag_report_t* r, const uint8_t* scanner_mac)
{
	if (!r)
		return;

	bmt_zone_lock();
	/* Check before get_or_add to know whether this is a new tag - used
	 * to decide connect / set_role against TB below. */
	bool was_new = (bmt_zone_track_find(r->tag_id) == NULL);
	bmt_tag_track_t* t = bmt_zone_track_get_or_add(r->tag_id, r->battery);
	if (!t)
	{
		bmt_zone_unlock();
		return;
	}
	/* A fresh report supersedes an out-of-range update that could not be sent
	 * while MQTT was disconnected. */
	t->out_of_range_pending = false;
	if (r->scanner_id >= 1 && r->scanner_id <= BMT_MAX_SCANNERS)
	{
		int sidx = r->scanner_id - 1;
		uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
		t->rssi_by_scanner[sidx] = r->rssi;
		t->ts_by_scanner[sidx] = now;
		t->valid_by_scanner[sidx] = true;
		t->last_any_report_ms = now;

		/* Real zone is computed in the TB rule chain. The gateway only
		 * tracks "did we hear from this tag recently" so
		 * zone_timeout_task knows when to publish OUT_OF_RANGE. Any
		 * scanner_id != UNKNOWN is enough for that. */
		if (t->current_zone_id == BMT_ZONE_UNKNOWN)
		{
			t->current_zone_id = r->scanner_id;
			t->last_zone_change_ms = now;
		}
	}
	bmt_zone_unlock();

	if (!bmt_mqtt_is_connected() || !bmt_mqtt_get_client())
		return;

	char dev[32], json[384];
	snprintf(dev, sizeof(dev), BMT_DEV_NAME_TAG_FMT, r->tag_id);

	if (was_new)
	{
		tb_connect_device(dev, BMT_PROFILE_TAG);
		tb_set_role(dev, BMT_ROLE_TAG);
	}

	char scanner_key[16];
	if (scanner_mac)
	{
		snprintf(scanner_key, sizeof(scanner_key), "%02x%02x%02x%02x%02x%02x",
		         scanner_mac[0], scanner_mac[1], scanner_mac[2],
		         scanner_mac[3], scanner_mac[4], scanner_mac[5]);
	}
	else
	{
		snprintf(scanner_key, sizeof(scanner_key), "id_0x%02x", r->scanner_id);
		ESP_LOGW("BMT_TB", "Could not resolve MAC for scanner_id=0x%02x, using fallback key", r->scanner_id);
	}

	snprintf(json, sizeof(json),
	         "{\"%s\":[{\"scanner_id\":\"%s\",\"battery\":%u,"
	         "\"rssi\":%d,\"distance\":%.2f,\"loss\":%u}]}",
	         dev, scanner_key, r->battery,
	         r->rssi, r->distance_dm / 10.0f, r->loss_pct);

	int msg_id = esp_mqtt_client_publish(bmt_mqtt_get_client(), "v1/gateway/telemetry", json, 0, 0, 0);
	if (msg_id >= 0)
	{
		bmt_mqtt_note_published();
		ESP_LOGI("BMT_TB", "TB [%s] scanner=%s rssi=%d battery=%u%%", dev, scanner_key, r->rssi, r->battery);
	}
}

void bmt_tb_pub_ota_result(uint16_t addr, uint8_t status)
{
	if (!bmt_mqtt_is_connected() || !bmt_mqtt_get_client())
		return;
	int idx = bmt_node_table_find(addr);
	if (idx < 0)
		return;
	const bmt_node_t* n = bmt_node_table_get(idx);
	char dev[32], json[128];
	snprintf(dev, sizeof(dev), BMT_DEV_NAME_NODE_FMT,
	         n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5]);
	snprintf(json, sizeof(json), "{\"%s\":[{\"ota_result\":\"%s\"}]}",
	         dev, status == 0 ? "SUCCESS" : "FAILED");
	esp_mqtt_client_publish(bmt_mqtt_get_client(), "v1/gateway/telemetry", json, 0, 1, 0);
}

static void zone_timeout_task(void* arg)
{
	(void)arg;
	while (1)
	{
		vTaskDelay(pdMS_TO_TICKS(1000));
		uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
		bmt_zone_lock();
		for (int i = 0; i < bmt_zone_track_capacity(); i++)
		{
			bmt_tag_track_t* t = bmt_zone_track_get(i);
			if (!t || !t->active)
				continue;
			if ((now - t->last_any_report_ms) <= BMT_TAG_OUT_OF_RANGE_MS)
				continue;
			if (t->current_zone_id == BMT_ZONE_UNKNOWN)
				continue;
			ESP_LOGW("BMT_TB", "Tag 0x%04x OUT OF RANGE", t->tag_id);
			t->current_zone_id = BMT_ZONE_UNKNOWN;
			t->out_of_range_pending = true;
			t->last_zone_change_ms = now;
			for (int j = 0; j < BMT_MAX_SCANNERS; j++)
				t->valid_by_scanner[j] = false;
			if (tb_publish_tag_out_of_range(t->tag_id))
				t->out_of_range_pending = false;
		}
		bmt_zone_unlock();
	}
}

void bmt_tb_start_zone_timeout_task(void)
{
	assert(xTaskCreate(zone_timeout_task, "bmt_zone_timer", 3072, NULL, 3, NULL) == pdPASS);
}
