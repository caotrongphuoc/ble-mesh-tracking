#include "bmt_mesh.h"

#include "esp_log.h"
#include "esp_system.h"

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_health_model_api.h"

#include "freertos/task.h"

#include "bmt_types.h"
#include "bmt_config.h"
#include "bmt_ota.h"

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
static const uint8_t s_health_test_ids[] = {ESP_BLE_MESH_HEALTH_STANDARD_TEST};

static esp_ble_mesh_health_srv_t s_health_server = {
    .health_test = {
        .id_count = 1,
        .test_ids = s_health_test_ids,
        .company_id = BMT_CID_ESP,
    },
};

ESP_BLE_MESH_HEALTH_PUB_DEFINE(s_health_pub, 0, ROLE_NODE);

static esp_ble_mesh_cfg_srv_t s_cfg_server = {
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay = ESP_BLE_MESH_RELAY_ENABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_ENABLED,
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .default_ttl = 7,
};

/* Vendor server model — receives RESET_CMD / OTA_TRIGGER and
 * publishes OTA_RESULT. */
ESP_BLE_MESH_MODEL_PUB_DEFINE(s_vnd_pub, sizeof(bmt_ota_result_t) + 4, ROLE_NODE);

static esp_ble_mesh_model_op_t s_vnd_ops[] = {
    ESP_BLE_MESH_MODEL_OP(BMT_OP_VND_RESET_CMD, 1),
    ESP_BLE_MESH_MODEL_OP(BMT_OP_VND_OTA_TRIGGER, 1),
    ESP_BLE_MESH_MODEL_OP(BMT_OP_VND_OTA_RESULT, sizeof(bmt_ota_result_t)),
    ESP_BLE_MESH_MODEL_OP_END,
};

static esp_ble_mesh_model_t s_vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(BMT_CID_ESP, BMT_VND_MODEL_ID,
                              s_vnd_ops, &s_vnd_pub, NULL),
};

static esp_ble_mesh_model_t s_root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&s_cfg_server),
    ESP_BLE_MESH_MODEL_HEALTH_SRV(&s_health_server, &s_health_pub),
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
    .uuid = BMT_RELAY_UUID,
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
		ESP_LOGI(TAG, "Provision complete! addr=0x%04x net_idx=0x%04x",
		         s_node_addr, s_net_idx);
		xEventGroupSetBits(s_mesh_evgrp, BMT_PROV_COMPLETE_BIT);
		break;

	case ESP_BLE_MESH_NODE_PROV_RESET_EVT:
		ESP_LOGI(TAG, "Node reset -> unprovisioned");
		s_node_addr = 0x0000;
		s_net_idx = 0xFFFF;
		s_app_idx = 0xFFFF;
		s_provisioned = false;
		xEventGroupClearBits(s_mesh_evgrp, BMT_PROV_COMPLETE_BIT);
		if (s_reboot_after_reset)
		{
			ESP_LOGW(TAG, "[RESET] Local reset COMPLETE — rebooting now");
			s_reboot_after_reset = false;
			vTaskDelay(pdMS_TO_TICKS(300));
			esp_restart();
			return;
		}
		esp_ble_mesh_node_prov_enable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
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
		ESP_LOGI(TAG, "AppKey received idx=0x%04x", s_app_idx);
		break;
	case ESP_BLE_MESH_MODEL_OP_RELAY_SET:
		ESP_LOGI(TAG, "Relay state changed by config client");
		break;
	default:
		break;
	}
}

static void mesh_health_server_cb(esp_ble_mesh_health_server_cb_event_t event,
                                  esp_ble_mesh_health_server_cb_param_t* param)
{
	(void)param;
	ESP_LOGI(TAG, "Health event: %d", event);
}

static void mesh_custom_model_cb(esp_ble_mesh_model_cb_event_t event,
                                 esp_ble_mesh_model_cb_param_t* param)
{
	if (event != ESP_BLE_MESH_MODEL_OPERATION_EVT || !param || !param->model_operation.ctx)
		return;

	uint32_t opcode = param->model_operation.opcode;
	uint16_t src = param->model_operation.ctx->addr;

	if (opcode == BMT_OP_VND_OTA_TRIGGER)
	{
		ESP_LOGW(TAG, "[OTA] OTA_TRIGGER from 0x%04x — starting WiFi OTA", src);
		bmt_ota_trigger();
		return;
	}
	if (opcode == BMT_OP_VND_RESET_CMD)
	{
		ESP_LOGW(TAG, "[VND] RESET_CMD from gateway — resetting mesh, will reboot when done...");
		vTaskDelay(pdMS_TO_TICKS(300));
		bmt_mesh_local_reset();
	}
}
esp_err_t bmt_mesh_init(void)
{
	s_mesh_evgrp = xEventGroupCreate();

	esp_ble_mesh_register_prov_callback(mesh_prov_cb);
	esp_ble_mesh_register_config_server_callback(mesh_cfg_server_cb);
	esp_ble_mesh_register_health_server_callback(mesh_health_server_cb);
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
	}
	else
	{
		err = esp_ble_mesh_node_prov_enable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
		if (err != ESP_OK)
			return err;
		ESP_LOGI(TAG, "BLE Mesh Relay initialized");
		ESP_LOGI(TAG, "UUID: RELAY ...:%02X | Waiting provision...", BMT_RELAY_UUID[15]);
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
uint16_t bmt_mesh_net_idx(void)
{
	return s_net_idx;
}
uint16_t bmt_mesh_app_idx(void)
{
	return s_app_idx;
}
bool bmt_mesh_relay_enabled(void)
{
	return s_cfg_server.relay == ESP_BLE_MESH_RELAY_ENABLED;
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
		              "forcing reboot");
		esp_restart();
	}
	vTaskDelete(NULL);
}

void bmt_mesh_local_reset(void)
{
	s_reboot_after_reset = true;
	xTaskCreate(reset_reboot_fallback_task, "bmt_rst_fb", 2048, NULL, 3, NULL);
	esp_ble_mesh_node_local_reset();
}
