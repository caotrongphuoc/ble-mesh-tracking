#include <assert.h>
#include "bmt_mesh.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_generic_model_api.h"

#include "freertos/task.h"

#include "bmt_types.h"
#include "bmt_tag_table.h"
#include "bmt_auth.h"
#include "bmt_ota.h"
#include "bmt_scan_core.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

static const char* TAG = "BMT_MESH";

static uint16_t s_node_addr = 0x0000;
static uint16_t s_net_idx = 0xFFFF;
static uint16_t s_app_idx = 0xFFFF;
static bool s_provisioned = false;
static EventGroupHandle_t s_mesh_evgrp;
static volatile bool s_reboot_after_reset = false;
static uint8_t s_scan_uuid[16] = {
    0x53,
    0x43,
    0x41,
    0x4E, /* "SCAN" — Gateway looks at these bytes to identify a scanner. */
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
static esp_ble_mesh_cfg_srv_t s_cfg_server = {
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay = ESP_BLE_MESH_RELAY_ENABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_ENABLED,
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .default_ttl = 7,
};

ESP_BLE_MESH_MODEL_PUB_DEFINE(s_vnd_pub, sizeof(bmt_tag_report_t) + 4, ROLE_NODE);

static esp_ble_mesh_model_op_t s_vnd_ops[] = {
    ESP_BLE_MESH_MODEL_OP(BMT_OP_VND_TAG_STATUS, sizeof(bmt_tag_report_t)),
    ESP_BLE_MESH_MODEL_OP(BMT_OP_VND_OTA_TRIGGER, 1),
    ESP_BLE_MESH_MODEL_OP(BMT_OP_VND_RESET_CMD, 1),
    ESP_BLE_MESH_MODEL_OP(BMT_OP_VND_OTA_RESULT, sizeof(bmt_ota_result_t)),
    ESP_BLE_MESH_MODEL_OP_END,
};

static esp_ble_mesh_model_t s_vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(BMT_CID_ESP, BMT_VND_MODEL_ID,
                              s_vnd_ops, &s_vnd_pub, NULL),
};

static esp_ble_mesh_model_t s_root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&s_cfg_server),
};

static esp_ble_mesh_elem_t s_elements[] = {
    ESP_BLE_MESH_ELEMENT(0, s_root_models, s_vnd_models),
};

static esp_ble_mesh_comp_t s_composition = {
    .cid = BMT_CID_ESP,
    .element_count = ARRAY_SIZE(s_elements),
    .elements = s_elements,
};
static const uint8_t BMT_MESH_STATIC_OOB_VAL[16] = {
    0x8E, 0x2F, 0x71, 0xC4, 0x3A, 0x95, 0xD6, 0x0B,
    0x47, 0xE8, 0x1C, 0x63, 0xAF, 0x29, 0x5D, 0x92};

static esp_ble_mesh_prov_t s_provision = {
    .uuid = s_scan_uuid,
    .static_val = BMT_MESH_STATIC_OOB_VAL,
    .static_val_len = sizeof(BMT_MESH_STATIC_OOB_VAL),
};
static void mesh_prov_cb(esp_ble_mesh_prov_cb_event_t event,
                         esp_ble_mesh_prov_cb_param_t* param)
{
	switch (event)
	{
	case ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT:
		s_net_idx = param->node_prov_complete.net_idx;
		s_node_addr = param->node_prov_complete.addr;
		s_provisioned = true;
		ESP_LOGI(TAG, "Provisioned! addr=0x%04x", s_node_addr);
		xEventGroupSetBits(s_mesh_evgrp, BMT_PROV_COMPLETE_BIT);
		break;
	case ESP_BLE_MESH_NODE_PROV_RESET_EVT:
		s_node_addr = 0x0000;
		s_net_idx = 0xFFFF;
		s_app_idx = 0xFFFF;
		s_provisioned = false;
		xEventGroupClearBits(s_mesh_evgrp, BMT_PROV_COMPLETE_BIT);
		if (s_reboot_after_reset)
		{
			/* Mesh NVS reset has REALLY completed (this event proves it)
			 * — reboot is now safe; no more blind 500 ms delay. */
			ESP_LOGW(TAG, "[RESET] Local reset COMPLETE — rebooting now");
			s_reboot_after_reset = false;
			vTaskDelay(pdMS_TO_TICKS(300));
			esp_restart();
			return;
		}
		esp_ble_mesh_node_prov_enable(
		    ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
		break;
	default:
		break;
	}
}

static void mesh_cfg_server_cb(esp_ble_mesh_cfg_server_cb_event_t event,
                               esp_ble_mesh_cfg_server_cb_param_t* param)
{
	if (event != ESP_BLE_MESH_CFG_SERVER_STATE_CHANGE_EVT)
		return;
	switch (param->ctx.recv_op)
	{
	case ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD:
		s_app_idx = param->value.state_change.appkey_add.app_idx;
		ESP_LOGI(TAG, "=== AppKey received! idx=0x%04x — Ready! ===", s_app_idx);
		break;
	case ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND:
		ESP_LOGI(TAG, "Model AppKey bind done");
		break;
	default:
		break;
	}
}

static void mesh_custom_model_cb(esp_ble_mesh_model_cb_event_t event,
                                 esp_ble_mesh_model_cb_param_t* param)
{
	if (event != ESP_BLE_MESH_MODEL_OPERATION_EVT || !param || !param->model_operation.ctx)
		return;

	uint32_t opcode = param->model_operation.opcode;
	uint16_t src = param->model_operation.ctx->addr;

	/* OTA_TRIGGER: on receipt, have bmt_ota spawn its own WiFi task.
	 * Do NOT do the OTA inside this callback — it would block the
	 * BLE host task. */
	if (opcode == BMT_OP_VND_OTA_TRIGGER)
	{
		ESP_LOGW(TAG, "[OTA] OTA_TRIGGER received from 0x%04x — starting WiFi OTA", src);
		bmt_ota_trigger();
		return;
	}
	if (opcode == BMT_OP_VND_RESET_CMD)
	{
		/* Reuse bmt_mesh_local_reset() — reboots exactly when the mesh
		 * reset really completes via event, no more blind fixed delay. */
		ESP_LOGW(TAG, "[VND] RESET_CMD — resetting mesh, will reboot when done...");
		vTaskDelay(pdMS_TO_TICKS(300));
		bmt_mesh_local_reset();
		return;
	}
	if (opcode == BMT_OP_VND_OTA_KEY_PUSH)
	{
		if (param->model_operation.length == 16)
		{
			ESP_LOGW(TAG, "[SECURITY] OTA_KEY_PUSH received from 0x%04x — rotating beacon key", src);
			bmt_auth_set_ota_beacon_key(param->model_operation.msg, 16);
		}
		else
		{
			ESP_LOGE(TAG, "[SECURITY] OTA_KEY_PUSH bad length: %d", param->model_operation.length);
		}
		return;
	}
}
esp_err_t bmt_mesh_init(void)
{
	s_mesh_evgrp = xEventGroupCreate();

	uint8_t mac[6];
	esp_read_mac(mac, ESP_MAC_BT);
	memcpy(&s_scan_uuid[4], mac, 6);
	s_scan_uuid[15] = bmt_scan_core_scanner_id();

	ESP_LOGI(TAG, "MAC  : %02X:%02X:%02X:%02X:%02X:%02X",
	         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	ESP_LOGI(TAG, "UUID : SCAN+%02X%02X%02X%02X%02X%02X+...+%02X",
	         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
	         bmt_scan_core_scanner_id());

	esp_ble_mesh_register_prov_callback(mesh_prov_cb);
	esp_ble_mesh_register_config_server_callback(mesh_cfg_server_cb);
	esp_ble_mesh_register_custom_model_callback(mesh_custom_model_cb);

	esp_err_t err = esp_ble_mesh_init(&s_provision, &s_composition);
	if (err != ESP_OK)
		return err;

	if (esp_ble_mesh_node_is_provisioned())
	{
		ESP_LOGI(TAG, "Already provisioned (restored from NVS)");
		s_provisioned = true;
		s_app_idx = 0x0000;
		xEventGroupSetBits(s_mesh_evgrp, BMT_PROV_COMPLETE_BIT);
		ESP_LOGI(TAG, "[MESH] NVS restore OK | scanner_id=0x%02X | app_idx=0x%04X",
		         bmt_scan_core_scanner_id(), s_app_idx);
	}
	else
	{
		err = esp_ble_mesh_node_prov_enable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
		if (err != ESP_OK)
			return err;
		ESP_LOGI(TAG, "Scanner ID=0x%02X | Waiting provision...", bmt_scan_core_scanner_id());
	}
	return ESP_OK;
}

EventGroupHandle_t bmt_mesh_evgrp(void)
{
	return s_mesh_evgrp;
}
uint16_t bmt_mesh_node_addr(void)
{
	return s_node_addr;
}
uint16_t bmt_mesh_app_idx(void)
{
	return s_app_idx;
}
const uint8_t* bmt_mesh_uuid(void)
{
	return s_scan_uuid;
}

void bmt_mesh_publish_tags(void)
{
	if (!s_provisioned)
		return;
	if (s_app_idx == 0xFFFF)
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
		    .scanner_id = bmt_scan_core_scanner_id(),
		    .battery = t->battery,
		    .tag_id = t->tag_id,
		    .rssi = (int8_t)t->rssi_filtered,
		    .distance_dm = dist_dm,
		    .loss_pct = loss_pct,
		};

		s_vnd_models[0].pub->publish_addr = 0x0001;
		s_vnd_models[0].pub->app_idx = s_app_idx;
		s_vnd_models[0].pub->ttl = 7;

		esp_err_t err = esp_ble_mesh_model_publish(
		    &s_vnd_models[0], BMT_OP_VND_TAG_STATUS,
		    sizeof(report), (uint8_t*)&report, ROLE_NODE);

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

esp_err_t bmt_mesh_report_ota_result(uint8_t status)
{
	if (!s_provisioned || s_app_idx == 0xFFFF)
	{
		ESP_LOGW(TAG, "[OTA] Not provisioned, cannot report OTA result");
		return ESP_ERR_INVALID_STATE;
	}
	bmt_ota_result_t r = {.status = status};
	s_vnd_models[0].pub->publish_addr = 0x0001;
	s_vnd_models[0].pub->app_idx = s_app_idx;
	s_vnd_models[0].pub->ttl = 7;
	esp_err_t e = esp_ble_mesh_model_publish(&s_vnd_models[0], BMT_OP_VND_OTA_RESULT,
	                                         sizeof(r), (uint8_t*)&r, ROLE_NODE);
	ESP_LOGI(TAG, "[OTA] OTA result report status=%u: %s", status,
	         e == ESP_OK ? "sent" : esp_err_to_name(e));
	return e;
}

static void reset_reboot_fallback_task(void* arg)
{
	(void)arg;
	vTaskDelay(pdMS_TO_TICKS(5000));
	if (s_reboot_after_reset)
	{
		ESP_LOGW(TAG, "[RESET] Fallback timeout — reset event did not arrive within 5s, "
		              "forcing reboot (NVS may not have been fully written yet)");
		esp_restart();
	}
	vTaskDelete(NULL);
}

void bmt_mesh_local_reset(void)
{
	s_reboot_after_reset = true;
	assert(xTaskCreate(reset_reboot_fallback_task, "bmt_rst_fb", 2048, NULL, 3, NULL) == pdPASS);
	esp_ble_mesh_node_local_reset();
}
