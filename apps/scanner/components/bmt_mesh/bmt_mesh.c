#include "bmt_mesh.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"

#include "esp_ble_mesh_defs.h"

#include "freertos/task.h"

#include "bmt_types.h"
#include "bmt_tag_table.h"
#include "bmt_auth.h"
#include "bmt_ota.h"
#include "bmt_scanner_id.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

static const char* TAG = "BMT_MESH";

static uint8_t s_scan_uuid[16] = {
    0x53,
    0x43,
    0x41,
    0x4E, /* "SCAN" - Gateway looks at these bytes to identify a scanner. */
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
};
static esp_ble_mesh_cfg_srv_t s_cfg_server = BMT_MESH_NODE_CFG_SRV_INIT;

ESP_BLE_MESH_MODEL_PUB_DEFINE(s_vnd_pub, sizeof(bmt_tag_report_t) + 4, ROLE_NODE);

/* Every opcode the scanner must receive has to be listed here - the
 * mesh access layer drops any opcode missing from the model's op table
 * before the custom model callback runs. */
static esp_ble_mesh_model_op_t s_vnd_ops[] = {
    ESP_BLE_MESH_MODEL_OP(BMT_OP_VND_TAG_STATUS, sizeof(bmt_tag_report_t)),
    ESP_BLE_MESH_MODEL_OP(BMT_OP_VND_OTA_TRIGGER, 1),
    ESP_BLE_MESH_MODEL_OP(BMT_OP_VND_RESET_CMD, 1),
    ESP_BLE_MESH_MODEL_OP(BMT_OP_VND_OTA_RESULT, sizeof(bmt_ota_result_t)),
    ESP_BLE_MESH_MODEL_OP(BMT_OP_VND_OTA_KEY_PUSH, 16),
    ESP_BLE_MESH_MODEL_OP_END,
};

static esp_ble_mesh_model_t s_vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(BMT_CID_ESPRESSIF, BMT_VND_MODEL_ID,
                              s_vnd_ops, &s_vnd_pub, NULL),
};

static esp_ble_mesh_model_t s_root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&s_cfg_server),
};

static esp_ble_mesh_elem_t s_elements[] = {
    ESP_BLE_MESH_ELEMENT(0, s_root_models, s_vnd_models),
};

static esp_ble_mesh_comp_t s_composition = {
    .cid = BMT_CID_ESPRESSIF,
    .element_count = ARRAY_SIZE(s_elements),
    .elements = s_elements,
};
static const uint8_t BMT_MESH_STATIC_OOB_VAL[16] = BMT_MESH_STATIC_OOB_INIT;

static esp_ble_mesh_prov_t s_provision = {
    .uuid = s_scan_uuid,
    .static_val = BMT_MESH_STATIC_OOB_VAL,
    .static_val_len = sizeof(BMT_MESH_STATIC_OOB_VAL),
};

static void on_vnd_msg(uint32_t opcode, uint16_t src, const uint8_t* msg, uint16_t len)
{
	/* OTA_TRIGGER: on receipt, have bmt_ota spawn its own WiFi task.
	 * Do NOT do the OTA inside this callback - it would block the
	 * BLE host task. */
	if (opcode == BMT_OP_VND_OTA_TRIGGER)
	{
		ESP_LOGW(TAG, "[OTA] OTA_TRIGGER received from 0x%04x - starting WiFi OTA", src);
		bmt_ota_trigger();
		return;
	}
	if (opcode == BMT_OP_VND_OTA_KEY_PUSH)
	{
		if (len == 16)
		{
			ESP_LOGW(TAG, "[SECURITY] OTA_KEY_PUSH received from 0x%04x - rotating beacon key", src);
			bmt_auth_set_ota_beacon_key(msg, 16);
		}
		else
		{
			ESP_LOGE(TAG, "[SECURITY] OTA_KEY_PUSH bad length: %d", len);
		}
	}
}

esp_err_t bmt_mesh_init(void)
{
	uint8_t mac[6];
	esp_read_mac(mac, ESP_MAC_BT);
	memcpy(&s_scan_uuid[4], mac, 6);
	s_scan_uuid[15] = bmt_scanner_id();

	ESP_LOGI(TAG, "MAC  : %02X:%02X:%02X:%02X:%02X:%02X",
	         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	ESP_LOGI(TAG, "UUID : SCAN+%02X%02X%02X%02X%02X%02X+...+%02X",
	         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
	         bmt_scanner_id());

	esp_err_t err = bmt_mesh_node_start(&s_provision, &s_composition, &s_vnd_models[0], on_vnd_msg);
	if (err != ESP_OK)
		return err;

	if (bmt_mesh_is_provisioned())
		ESP_LOGI(TAG, "[MESH] NVS restore OK | scanner_id=0x%02X | app_idx=0x%04X",
		         bmt_scanner_id(), bmt_mesh_app_idx());
	else
		ESP_LOGI(TAG, "Scanner ID=0x%02X | Waiting provision...", bmt_scanner_id());
	return ESP_OK;
}

const uint8_t* bmt_mesh_uuid(void)
{
	return s_scan_uuid;
}

void bmt_mesh_publish_tags(void)
{
	if (!bmt_mesh_is_provisioned() || bmt_mesh_app_idx() == 0xFFFF)
		return;

	int published = 0;
	for (int i = 0; i < BMT_MAX_TAGS; i++)
	{
		const bmt_scan_tag_info_t* t = bmt_tag_table_at(i);
		if (!t)
			continue;

		uint32_t total = t->total_received + t->total_missed;
		uint8_t loss_pct = (total > 0) ? (uint8_t)(t->total_missed * 100 / total) : 0;
		int16_t dist_dm = (int16_t)(t->distance * 10.0f);
		if (dist_dm < 0)
			dist_dm = 0;

		bmt_tag_report_t report = {
		    .scanner_id = bmt_scanner_id(),
		    .battery = t->battery,
		    .tag_id = t->tag_id,
		    .rssi = (int8_t)t->rssi_filtered,
		    .distance_dm = dist_dm,
		    .loss_pct = loss_pct,
		};

		esp_err_t err = bmt_mesh_publish_to_gateway(BMT_OP_VND_TAG_STATUS, &report, sizeof(report));
		if (err == ESP_OK)
		{
			published++;
			ESP_LOGI(TAG, "[Mesh] Published 0x%04X rssi=%d dist=%.1fm loss=%u%%",
			         report.tag_id, report.rssi, dist_dm / 10.0f, loss_pct);
		}
		vTaskDelay(pdMS_TO_TICKS(50));
	}
	if (published == 0)
		ESP_LOGD(TAG, "[Mesh] No tags");
}
