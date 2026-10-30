#include <stdlib.h>
#include "bmt_mesh_node.h"

#include "esp_log.h"
#include "esp_system.h"

#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"

#include "freertos/task.h"

#include "bmt_protocol_mesh.h"

static const char* TAG = "BMT_MESH";

static uint16_t s_node_addr = 0x0000;
static uint16_t s_net_idx = 0xFFFF;
static uint16_t s_app_idx = 0xFFFF;
static bool s_provisioned = false;
static EventGroupHandle_t s_mesh_evgrp;
static volatile bool s_reboot_after_reset = false;
static esp_ble_mesh_model_t* s_vnd_model;
static bmt_mesh_node_vnd_cb_t s_on_vnd;

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
			/* Mesh NVS reset has REALLY completed (this event proves it)
			 * - reboot is safe now, without guessing a fixed delay. */
			ESP_LOGW(TAG, "[RESET] Local reset COMPLETE - rebooting now");
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
		ESP_LOGI(TAG, "=== AppKey received! idx=0x%04x - Ready! ===", s_app_idx);
		break;
	case ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND:
		ESP_LOGI(TAG, "Model AppKey bind done");
		break;
	case ESP_BLE_MESH_MODEL_OP_RELAY_SET:
		ESP_LOGI(TAG, "Relay state changed by config client");
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

	if (opcode == BMT_OP_VND_RESET_CMD)
	{
		/* Reuse bmt_mesh_local_reset() - reboots exactly when the mesh
		 * reset really completes (PROV_RESET event), not after a guessed delay. */
		ESP_LOGW(TAG, "[VND] RESET_CMD from 0x%04x - resetting mesh, will reboot when done...", src);
		vTaskDelay(pdMS_TO_TICKS(300));
		bmt_mesh_local_reset();
		return;
	}
	if (s_on_vnd)
		s_on_vnd(opcode, src, param->model_operation.msg, param->model_operation.length);
}

esp_err_t bmt_mesh_node_start(esp_ble_mesh_prov_t* prov, esp_ble_mesh_comp_t* comp,
                              esp_ble_mesh_model_t* vnd_model, bmt_mesh_node_vnd_cb_t on_vnd)
{
	s_mesh_evgrp = xEventGroupCreate();
	s_vnd_model = vnd_model;
	s_on_vnd = on_vnd;

	esp_ble_mesh_register_prov_callback(mesh_prov_cb);
	esp_ble_mesh_register_config_server_callback(mesh_cfg_server_cb);
	esp_ble_mesh_register_custom_model_callback(mesh_custom_model_cb);

	esp_err_t err = esp_ble_mesh_init(prov, comp);
	if (err != ESP_OK)
		return err;

	if (esp_ble_mesh_node_is_provisioned())
	{
		ESP_LOGI(TAG, "Already provisioned (restored from NVS)");
		s_provisioned = true;
		s_app_idx = 0x0000;
		xEventGroupSetBits(s_mesh_evgrp, BMT_PROV_COMPLETE_BIT);
		return ESP_OK;
	}
	return esp_ble_mesh_node_prov_enable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
}

EventGroupHandle_t bmt_mesh_evgrp(void)
{
	return s_mesh_evgrp;
}
bool bmt_mesh_is_provisioned(void)
{
	return s_provisioned;
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

esp_err_t bmt_mesh_publish_to_gateway(uint32_t opcode, const void* data, uint16_t len)
{
	if (!s_provisioned || s_app_idx == 0xFFFF || !s_vnd_model)
		return ESP_ERR_INVALID_STATE;
	s_vnd_model->pub->publish_addr = 0x0001;
	s_vnd_model->pub->app_idx = s_app_idx;
	s_vnd_model->pub->ttl = 7;
	return esp_ble_mesh_model_publish(s_vnd_model, opcode, len, (uint8_t*)data, ROLE_NODE);
}

esp_err_t bmt_mesh_report_ota_result(uint8_t status)
{
	bmt_ota_result_t r = {.status = status};
	esp_err_t e = bmt_mesh_publish_to_gateway(BMT_OP_VND_OTA_RESULT, &r, sizeof(r));
	if (e == ESP_ERR_INVALID_STATE)
	{
		ESP_LOGW(TAG, "[OTA] Not provisioned, cannot report OTA result");
		return e;
	}
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
		ESP_LOGW(TAG, "[RESET] Fallback timeout - reset event did not arrive within 5s, "
		              "forcing reboot (NVS may not have been fully written yet)");
		esp_restart();
	}
	vTaskDelete(NULL);
}

void bmt_mesh_local_reset(void)
{
	s_reboot_after_reset = true;
	if (xTaskCreate(reset_reboot_fallback_task, "bmt_rst_fb", 2048, NULL, 3, NULL) != pdPASS)
		abort();
	esp_ble_mesh_node_local_reset();
}
