#include "bmt_mesh.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"

#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "bmt_config.h"
#include "bmt_mac_cache.h"
#include "bmt_mqtt.h"
#include "bmt_node_table.h"
#include "bmt_ota.h"
#include "bmt_scan_list.h"
#include "bmt_thingsboard.h"
#include "bmt_types.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

static const char* TAG = "BMT_MESH";

#define BMT_NODE_PING_INTERVAL_MS 20000
#define BMT_NODE_OFFLINE_TIMEOUT_MS 60000
#define BMT_SCAN_STATUS_REFRESH_MS 30000

static uint16_t s_net_key_idx = 0x0000;
static uint16_t s_app_key_idx = 0x0000;

/* Randomised at first boot by bmt_mesh_generate_keys_if_needed(); the
 * mesh stack then persists them in its own NVS. NOT const because
 * written at runtime. */
static uint8_t s_net_key[16];
static uint8_t s_app_key[16];

static volatile uint32_t s_mesh_received = 0;

#define BMT_CFG_ACK_BIT BIT0
#define BMT_CFG_ACK_TIMEOUT_MS 10000
#define BMT_CFG_RETRY 3
static EventGroupHandle_t s_cfg_ack_evgrp = NULL;
static volatile uint16_t s_cfg_wait_addr = 0;
static volatile uint32_t s_cfg_wait_opcode = 0;

static SemaphoreHandle_t s_cfg_mutex = NULL;

static bool wait_cfg_ack(uint16_t addr, uint32_t opcode)
{
	if (!s_cfg_ack_evgrp)
		return false;
	xEventGroupClearBits(s_cfg_ack_evgrp, BMT_CFG_ACK_BIT);
	s_cfg_wait_addr = addr;
	s_cfg_wait_opcode = opcode;
	EventBits_t bits = xEventGroupWaitBits(s_cfg_ack_evgrp, BMT_CFG_ACK_BIT,
	                                       pdTRUE, pdFALSE,
	                                       pdMS_TO_TICKS(BMT_CFG_ACK_TIMEOUT_MS));
	s_cfg_wait_addr = 0;
	s_cfg_wait_opcode = 0;
	return (bits & BMT_CFG_ACK_BIT) != 0;
}

/* Gui 1 buoc config + cho ACK, thu lai toi da BMT_CFG_RETRY lan. APP_KEY_ADD va
 * MODEL_APP_BIND deu idempotent (gui lai cung gia tri -> node tra SUCCESS) nen
 * retry an toan. Tra ve true neu co ACK. */
static bool cfg_send_retry(const char* step, uint16_t addr, uint32_t opcode,
                           esp_ble_mesh_client_common_param_t* c,
                           esp_ble_mesh_cfg_client_set_state_t* s)
{
	for (int a = 1; a <= BMT_CFG_RETRY; a++)
	{
		esp_err_t e = esp_ble_mesh_config_client_set_state(c, s);
		if (e != ESP_OK)
		{
			ESP_LOGW(TAG, "%s send fail 0x%04x [%d/%d]: %s", step, addr, a, BMT_CFG_RETRY, esp_err_to_name(e));
			vTaskDelay(pdMS_TO_TICKS(800));
			continue;
		}
		if (wait_cfg_ack(addr, opcode))
		{
			ESP_LOGI(TAG, "%s ACK 0x%04x (lan %d)", step, addr, a);
			return true;
		}
		ESP_LOGW(TAG, "%s no ACK 0x%04x [%d/%d]", step, addr, a, BMT_CFG_RETRY);
		vTaskDelay(pdMS_TO_TICKS(500));
	}
	return false;
}

static void cfg_abort_cleanup(uint16_t addr)
{
	ESP_LOGE(TAG, "[CFG] Config 0x%04x THAT BAI sau %d lan — xoa entry (tranh zombie chan re-provision)",
	         addr, BMT_CFG_RETRY);
	int idx = bmt_node_table_find(addr);
	if (idx >= 0)
	{
		bmt_node_t* n = bmt_node_table_get(idx);
		esp_ble_mesh_provisioner_delete_node_with_uuid(n->uuid);
		memset(n, 0, sizeof(*n));
		bmt_node_table_save();
	}
}
static const uint8_t BMT_MESH_STATIC_OOB_VAL[16] = {
    0x8E, 0x2F, 0x71, 0xC4, 0x3A, 0x95, 0xD6, 0x0B,
    0x47, 0xE8, 0x1C, 0x63, 0xAF, 0x29, 0x5D, 0x92};
static esp_ble_mesh_cfg_srv_t s_cfg_server = {
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay = ESP_BLE_MESH_RELAY_ENABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_ENABLED,
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .default_ttl = 7,
};

static esp_ble_mesh_client_t s_cfg_client;
static esp_ble_mesh_client_t s_vnd_client;
ESP_BLE_MESH_MODEL_PUB_DEFINE(s_vnd_pub, 20, ROLE_PROVISIONER);

static esp_ble_mesh_model_op_t s_vnd_ops[] = {
    ESP_BLE_MESH_MODEL_OP(BMT_OP_VND_TAG_STATUS, sizeof(bmt_tag_report_t)),
    ESP_BLE_MESH_MODEL_OP(BMT_OP_VND_OTA_RESULT, sizeof(bmt_ota_result_t)),
    ESP_BLE_MESH_MODEL_OP_END,
};

static esp_ble_mesh_model_t s_vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(BMT_CID_ESP, BMT_VND_MODEL_ID, s_vnd_ops, &s_vnd_pub, &s_vnd_client),
};

static esp_ble_mesh_model_t s_root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&s_cfg_server),
    ESP_BLE_MESH_MODEL_CFG_CLI(&s_cfg_client),
};

static esp_ble_mesh_elem_t s_elements[] = {
    ESP_BLE_MESH_ELEMENT(0, s_root_models, s_vnd_models),
};

static esp_ble_mesh_comp_t s_composition = {
    .cid = BMT_CID_ESP,
    .element_count = ARRAY_SIZE(s_elements),
    .elements = s_elements,
};

static esp_ble_mesh_prov_t s_provision = {
    .prov_unicast_addr = 0x0001,
    .prov_start_address = 0x0002,
};
void bmt_mesh_generate_keys_if_needed(void)
{
	const uint8_t* exist_net = esp_ble_mesh_provisioner_get_local_net_key(s_net_key_idx);
	if (!exist_net)
	{
		esp_fill_random(s_net_key, sizeof(s_net_key));
		esp_fill_random(s_app_key, sizeof(s_app_key));
		ESP_LOGI(TAG, "[SECURITY] Generating candidate key (used only if stack has no key after init)");
	}
	else
	{
		ESP_LOGI(TAG, "[SECURITY] Mesh keys already exist, skip generate");
	}
}
static void scan_config_task(void* arg)
{
	uint16_t addr = (uint16_t)(uint32_t)arg;
	if (s_cfg_mutex)
		xSemaphoreTake(s_cfg_mutex, portMAX_DELAY); /* serialize: 1 config task/luc */
	ESP_LOGI(TAG, "[SCN_CFG] Configuring scan node 0x%04x...", addr);
	vTaskDelay(pdMS_TO_TICKS(2000));

	esp_ble_mesh_client_common_param_t c = {0};
	esp_ble_mesh_cfg_client_set_state_t s = {0};
	c.opcode = ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD;
	c.model = &s_root_models[1];
	c.ctx.net_idx = s_net_key_idx;
	c.ctx.app_idx = 0xFFFF;
	c.ctx.addr = addr;
	c.ctx.send_ttl = 7;
	c.msg_timeout = 8000;
	s.app_key_add.net_idx = s_net_key_idx;
	s.app_key_add.app_idx = s_app_key_idx;
	memcpy(s.app_key_add.app_key, s_app_key, 16);
	if (!cfg_send_retry("[SCN_CFG] Step1 APP_KEY_ADD", addr, ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD, &c, &s))
	{
		cfg_abort_cleanup(addr);
		if (s_cfg_mutex)
			xSemaphoreGive(s_cfg_mutex);
		vTaskDelete(NULL);
		return;
	}

	memset(&c, 0, sizeof(c));
	memset(&s, 0, sizeof(s));
	c.opcode = ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND;
	c.model = &s_root_models[1];
	c.ctx.net_idx = s_net_key_idx;
	c.ctx.app_idx = 0xFFFF;
	c.ctx.addr = addr;
	c.ctx.send_ttl = 7;
	c.msg_timeout = 5000;
	s.model_app_bind.element_addr = addr;
	s.model_app_bind.model_app_idx = s_app_key_idx;
	s.model_app_bind.model_id = BMT_VND_MODEL_ID;
	s.model_app_bind.company_id = BMT_CID_ESP;
	if (!cfg_send_retry("[SCN_CFG] Step2 MODEL_APP_BIND", addr, ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND, &c, &s))
	{
		cfg_abort_cleanup(addr);
		if (s_cfg_mutex)
			xSemaphoreGive(s_cfg_mutex);
		vTaskDelete(NULL);
		return;
	}

	int idx = bmt_node_table_find(addr);
	if (idx >= 0)
	{
		bmt_node_table_get(idx)->config_done = true;
		bmt_node_table_save();
	}
	ESP_LOGI(TAG, "[SCN_CFG] Scan node 0x%04x fully configured!", addr);

	bmt_ota_push_beacon_key_to_node(addr);

	bmt_node_table_print();
	if (s_cfg_mutex)
		xSemaphoreGive(s_cfg_mutex);
	vTaskDelete(NULL);
}
static void relay_config_task(void* arg)
{
	uint16_t addr = (uint16_t)(uint32_t)arg;
	if (s_cfg_mutex)
		xSemaphoreTake(s_cfg_mutex, portMAX_DELAY); /* serialize: 1 config task/luc */
	ESP_LOGI(TAG, "[RLY_CFG] Configuring relay node 0x%04x...", addr);
	vTaskDelay(pdMS_TO_TICKS(2000));

	esp_ble_mesh_client_common_param_t c = {0};
	esp_ble_mesh_cfg_client_set_state_t s = {0};
	c.opcode = ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD;
	c.model = &s_root_models[1];
	c.ctx.net_idx = s_net_key_idx;
	c.ctx.app_idx = 0xFFFF;
	c.ctx.addr = addr;
	c.ctx.send_ttl = 7;
	c.msg_timeout = 8000;
	s.app_key_add.net_idx = s_net_key_idx;
	s.app_key_add.app_idx = s_app_key_idx;
	memcpy(s.app_key_add.app_key, s_app_key, 16);
	if (!cfg_send_retry("[RLY_CFG] Step1 APP_KEY_ADD", addr, ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD, &c, &s))
	{
		cfg_abort_cleanup(addr);
		if (s_cfg_mutex)
			xSemaphoreGive(s_cfg_mutex);
		vTaskDelete(NULL);
		return;
	}

	memset(&c, 0, sizeof(c));
	memset(&s, 0, sizeof(s));
	c.opcode = ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND;
	c.model = &s_root_models[1];
	c.ctx.net_idx = s_net_key_idx;
	c.ctx.app_idx = 0xFFFF;
	c.ctx.addr = addr;
	c.ctx.send_ttl = 7;
	c.msg_timeout = 5000;
	s.model_app_bind.element_addr = addr;
	s.model_app_bind.model_app_idx = s_app_key_idx;
	s.model_app_bind.model_id = BMT_VND_MODEL_ID;
	s.model_app_bind.company_id = BMT_CID_ESP;
	if (!cfg_send_retry("[RLY_CFG] Step2 MODEL_APP_BIND", addr, ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND, &c, &s))
	{
		cfg_abort_cleanup(addr);
		if (s_cfg_mutex)
			xSemaphoreGive(s_cfg_mutex);
		vTaskDelete(NULL);
		return;
	}

	int idx = bmt_node_table_find(addr);
	if (idx >= 0)
	{
		bmt_node_table_get(idx)->config_done = true;
		bmt_node_table_save();
	}
	ESP_LOGI(TAG, "[RLY_CFG] Relay 0x%04x fully configured — RESET_CMD enabled!", addr);
	bmt_node_table_print();
	if (s_cfg_mutex)
		xSemaphoreGive(s_cfg_mutex);
	vTaskDelete(NULL);
}
static void mesh_prov_cb(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t* param)
{
	switch (event)
	{
	case ESP_BLE_MESH_PROV_REGISTER_COMP_EVT:
		ESP_LOGI(TAG, "Provisioner registered");
		break;
	case ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT:
		ESP_LOGI(TAG, "Provisioner scan enabled");
		break;
	case ESP_BLE_MESH_PROVISIONER_SET_STATIC_OOB_VALUE_COMP_EVT:
		if (param->provisioner_set_static_oob_val_comp.err_code == 0)
			ESP_LOGI(TAG, "[SECURITY] Static OOB value set OK");
		else
			ESP_LOGE(TAG, "[SECURITY] Static OOB value set FAILED: %d",
			         param->provisioner_set_static_oob_val_comp.err_code);
		break;

	case ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT:
	{
		const uint8_t* uuid = param->provisioner_recv_unprov_adv_pkt.dev_uuid;
		const uint8_t* mac = param->provisioner_recv_unprov_adv_pkt.addr;
		uint8_t addr_type = param->provisioner_recv_unprov_adv_pkt.addr_type;
		uint16_t oob_info = param->provisioner_recv_unprov_adv_pkt.oob_info;
		bmt_mac_cache_store(uuid, mac);

		if (bmt_scan_list_get_mode() == BMT_PROV_MODE_MANUAL)
		{
			if (!bmt_scan_list_is_scanning())
				break;
			if (bmt_scan_list_add(uuid, mac, addr_type, oob_info))
			{
				printf("[SCAN] %-7s MAC:", bmt_uuid_type_str(uuid));
				for (int b = 0; b < 6; b++)
					printf("%02X%s", mac[b], b < 5 ? ":" : "");
				printf("\n");
			}
			break;
		}

		{
			int stale = -1;
			for (int i = 0; i < bmt_node_table_capacity(); i++)
			{
				const bmt_node_t* nn = bmt_node_table_get(i);
				if (nn && nn->used && memcmp(nn->uuid, uuid, 16) == 0)
				{
					stale = i;
					break;
				}
			}
			if (stale >= 0)
			{
				bmt_node_t* nn = bmt_node_table_get(stale);
				if (!nn->config_done)
					break; /* dang config do — bo qua nhu cu */
				ESP_LOGW(TAG, "Node 0x%04x (%s) beacon unprovisioned tro lai — "
				              "node da tu reset, xoa entry cu va provision lai",
				         nn->addr, nn->name);
				esp_ble_mesh_provisioner_delete_node_with_uuid(uuid);
				memset(nn, 0, sizeof(*nn));
				bmt_node_table_save();
			}
		}
		ESP_LOGI(TAG, "Found unprovisioned [%s]", bmt_uuid_type_str(uuid));
		esp_ble_mesh_unprov_dev_add_t dev = {0};
		memcpy(dev.uuid, uuid, 16);
		memcpy(dev.addr, mac, 6);
		dev.addr_type = addr_type;
		dev.oob_info = oob_info;
		dev.bearer = ESP_BLE_MESH_PROV_ADV;
		{
			esp_err_t e = esp_ble_mesh_provisioner_add_unprov_dev(&dev, ADD_DEV_FLUSHABLE_DEV_FLAG | ADD_DEV_START_PROV_NOW_FLAG);
			if (e != ESP_OK)
				ESP_LOGW(TAG, "add_unprov_dev fail [%s]: %s", bmt_uuid_type_str(uuid), esp_err_to_name(e));
		}
		break;
	}

	case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT:
	{
		uint16_t addr = param->provisioner_prov_complete.unicast_addr;
		const uint8_t* uuid = param->provisioner_prov_complete.device_uuid;
		ESP_LOGI(TAG, "Provision complete addr=0x%04x type=%s", addr, bmt_uuid_type_str(uuid));
		uint8_t mac[6] = {0};
		bmt_mac_cache_get(uuid, mac);
		int idx = bmt_node_table_add(addr, uuid, mac, NULL);
		if (idx < 0)
		{
			ESP_LOGW(TAG, "Node table full");
			break;
		}
		bmt_node_t* n = bmt_node_table_get(idx);

		if (bmt_uuid_is_relay(uuid))
		{
			n->is_relay = true;
			n->is_scan = false;
			n->config_done = false; /* wait for relay_config_task to finish */
			snprintf(n->name, sizeof(n->name), "Relay_0x%04x", addr);
			ESP_LOGI(TAG, "Node 0x%04x = RELAY, launching config task...", addr);
			bmt_node_table_save();
			bmt_node_table_print();
			xTaskCreate(relay_config_task, "relay_cfg", 3072, (void*)(uint32_t)addr, 5, NULL);
			break;
		}

		if (bmt_uuid_is_scan(uuid))
		{
			n->is_scan = true;
			n->is_relay = false;
			n->config_done = false;
			snprintf(n->name, sizeof(n->name), "Scan_0x%04x", addr);
			ESP_LOGI(TAG, "Node 0x%04x = SCAN, launching config task...", addr);
			bmt_node_table_save();
			bmt_node_table_print();
			xTaskCreate(scan_config_task, "scan_cfg", 3072, (void*)(uint32_t)addr, 5, NULL);
			break;
		}

		ESP_LOGW(TAG, "Unknown node type");
		bmt_node_table_save();
		bmt_node_table_print();
		break;
	}
	default:
		break;
	}
}

static void cfg_client_cb(esp_ble_mesh_cfg_client_cb_event_t event,
                          esp_ble_mesh_cfg_client_cb_param_t* param)
{
	if (!param || !param->params)
		return;
	uint16_t addr = param->params->ctx.addr;
	int idx = bmt_node_table_find(addr);
	bmt_node_t* n = (idx >= 0) ? bmt_node_table_get(idx) : NULL;

	if (event == ESP_BLE_MESH_CFG_CLIENT_SET_STATE_EVT)
	{
		uint32_t op = param->params->opcode;
		if (param->error_code == 0 && addr == s_cfg_wait_addr && op == s_cfg_wait_opcode && s_cfg_ack_evgrp)
			xEventGroupSetBits(s_cfg_ack_evgrp, BMT_CFG_ACK_BIT);
		if (op == ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD)
			ESP_LOGI(TAG, "[CFG] APP_KEY_ADD ACK from 0x%04x (err=%d)", addr, param->error_code);
		if (op == ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND)
		{
			ESP_LOGI(TAG, "[CFG] MODEL_APP_BIND ACK from 0x%04x (err=%d)", addr, param->error_code);
			if (param->error_code == 0)
			{
				if (n && n->is_scan)
					bmt_tb_pub_node_status(addr, n->mac, BMT_ROLE_SCAN, true);
				if (n && n->is_relay)
					bmt_tb_pub_node_status(addr, n->mac, BMT_ROLE_RELAY, true);
			}
		}
	}
	if (event == ESP_BLE_MESH_CFG_CLIENT_TIMEOUT_EVT)
		ESP_LOGW(TAG, "[CFG] TIMEOUT opcode=0x%04" PRIx32 " addr=0x%04x", param->params->opcode, addr);
	if (event == ESP_BLE_MESH_CFG_CLIENT_GET_STATE_EVT)
	{
		if (param->error_code == 0 && addr == s_cfg_wait_addr &&
		    param->params->opcode == s_cfg_wait_opcode && s_cfg_ack_evgrp)
			xEventGroupSetBits(s_cfg_ack_evgrp, BMT_CFG_ACK_BIT);
		if (param->error_code != 0)
		{
			ESP_LOGW(TAG, "[PING] 0x%04x FAILED (err=%d) — not counted as ACK",
			         addr, param->error_code);
			return;
		}
		if (n && n->config_done && (n->is_scan || n->is_relay))
		{
			n->last_seen_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
			s_mesh_received++;
			if (!n->online)
			{
				n->online = true;
				const char* role = n->is_scan ? BMT_ROLE_SCAN : BMT_ROLE_RELAY;
				ESP_LOGI(TAG, "%s 0x%04x ONLINE (ping)", role, addr);
				bmt_tb_pub_node_status(addr, n->mac, role, true);
			}
		}
	}
}

static void cfg_server_cb(esp_ble_mesh_cfg_server_cb_event_t event,
                          esp_ble_mesh_cfg_server_cb_param_t* param)
{
	(void)param;
	ESP_LOGI(TAG, "Config server event: %d", event);
}

/* Increment s_mesh_received when a TAG_STATUS is received from a
 * scanner. This counter is at the BLE Mesh layer; the watchdog uses it
 * to tell "mesh dead" apart from "only MQTT dead". */
static void vnd_client_cb(esp_ble_mesh_model_cb_event_t event,
                          esp_ble_mesh_model_cb_param_t* param)
{
	if (event != ESP_BLE_MESH_MODEL_OPERATION_EVT || !param || !param->model_operation.ctx)
		return;
	uint32_t opcode = param->model_operation.opcode;
	uint16_t src = param->model_operation.ctx->addr;
	const uint8_t* data = param->model_operation.msg;
	uint16_t len = param->model_operation.length;

	if (opcode == BMT_OP_VND_TAG_STATUS)
	{
		if (len < sizeof(bmt_tag_report_t))
		{
			ESP_LOGW(TAG, "[VND] TAG_STATUS too short");
			return;
		}
		bmt_tag_report_t report;
		memcpy(&report, data, sizeof(report));

		s_mesh_received++;

		const uint8_t* scanner_mac = NULL;
		int node_idx = bmt_node_table_find(src);
		bmt_node_t* scan_node = (node_idx >= 0) ? bmt_node_table_get(node_idx) : NULL;
		if (scan_node)
			scanner_mac = scan_node->mac;

		if (scan_node && scan_node->is_scan)
		{
			uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
			bool due = (scan_node->last_seen_ms == 0) ||
			           ((now - scan_node->last_seen_ms) > BMT_SCAN_STATUS_REFRESH_MS);
			if (due)
			{
				scan_node->last_seen_ms = now;
				scan_node->online = true;
				bmt_tb_pub_node_status(src, scan_node->mac, BMT_ROLE_SCAN, true);
			}
		}

		if (scanner_mac)
		{
			ESP_LOGI(TAG, "[VND] src=0x%04x MAC=%02x:%02x:%02x:%02x:%02x:%02x tag=0x%04x rssi=%d battery=%u%% (mesh_recv=%" PRIu32 ")",
			         src, scanner_mac[0], scanner_mac[1], scanner_mac[2],
			         scanner_mac[3], scanner_mac[4], scanner_mac[5],
			         report.tag_id, report.rssi, report.battery, s_mesh_received);
		}
		else
		{
			ESP_LOGW(TAG, "[VND] src=0x%04x MAC=? (chua tra ra) tag=0x%04x rssi=%d battery=%u%% (mesh_recv=%" PRIu32 ")",
			         src, report.tag_id, report.rssi, report.battery, s_mesh_received);
		}

		bmt_mqtt_enqueue_tag_report(&report, scanner_mac);
		return;
	}

	/* Node reports its own OTA result — replaces the earlier
	 * "fire-and-wait-fixed-time" approach that never actually learned
	 * whether the update succeeded. */
	if (opcode == BMT_OP_VND_OTA_RESULT)
	{
		if (len < sizeof(bmt_ota_result_t))
			return;
		bmt_ota_result_t r;
		memcpy(&r, data, sizeof(r));

		int idx = bmt_node_table_find(src);
		const char* name = (idx >= 0) ? bmt_node_table_get(idx)->name : "unknown";

		if (r.status == 0)
		{
			ESP_LOGI(TAG, "[OTA] ===== Node 0x%04x (%s) OTA THANH CONG =====", src, name);
		}
		else
		{
			ESP_LOGW(TAG, "[OTA] ===== Node 0x%04x (%s) OTA THAT BAI (status=%u) =====",
			         src, name, r.status);
		}
		bmt_tb_pub_ota_result(src, r.status);
		return;
	}
}
static void node_ping_task(void* arg)
{
	(void)arg;
	vTaskDelay(pdMS_TO_TICKS(30000));
	while (1)
	{
		for (int i = 0; i < bmt_node_table_capacity(); i++)
		{
			bmt_node_t* n = bmt_node_table_get(i);
			if (!n || !n->used || !n->config_done || (!n->is_scan && !n->is_relay))
				continue;
			esp_ble_mesh_client_common_param_t common = {0};
			esp_ble_mesh_cfg_client_get_state_t get = {0};
			common.opcode = ESP_BLE_MESH_MODEL_OP_DEFAULT_TTL_GET;
			common.model = &s_root_models[1];
			common.ctx.net_idx = s_net_key_idx;
			common.ctx.app_idx = 0xFFFF;
			common.ctx.addr = n->addr;
			common.ctx.send_ttl = 7;
			common.msg_timeout = 5000;
			if (s_cfg_mutex)
				xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
			esp_err_t pe = esp_ble_mesh_config_client_get_state(&common, &get);
			if (pe != ESP_OK)
				ESP_LOGW(TAG, "[PING] send fail to 0x%04x: %s", n->addr, esp_err_to_name(pe));
			else
				wait_cfg_ack(n->addr, ESP_BLE_MESH_MODEL_OP_DEFAULT_TTL_GET); /* cho het round-trip roi moi nha mutex */
			if (s_cfg_mutex)
				xSemaphoreGive(s_cfg_mutex);
			uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
			if (n->last_seen_ms > 0 && (now - n->last_seen_ms) > BMT_NODE_OFFLINE_TIMEOUT_MS)
			{
				if (n->online)
				{
					n->online = false;
					const char* role = n->is_scan ? BMT_ROLE_SCAN : BMT_ROLE_RELAY;
					ESP_LOGW(TAG, "%s 0x%04X OFFLINE", role, n->addr);
					bmt_tb_pub_node_status(n->addr, n->mac, role, false);
				}
			}
			vTaskDelay(pdMS_TO_TICKS(500));
		}
		vTaskDelay(pdMS_TO_TICKS(BMT_NODE_PING_INTERVAL_MS));
	}
}
esp_err_t bmt_mesh_init(void)
{
	esp_err_t err;
	s_cfg_ack_evgrp = xEventGroupCreate();
	s_cfg_mutex = xSemaphoreCreateMutex();
	esp_ble_mesh_register_prov_callback(mesh_prov_cb);
	esp_ble_mesh_register_config_client_callback(cfg_client_cb);
	esp_ble_mesh_register_config_server_callback(cfg_server_cb);
	esp_ble_mesh_register_custom_model_callback(vnd_client_cb);

	err = esp_ble_mesh_init(&s_provision, &s_composition);
	if (err != ESP_OK)
		return err;

	err = esp_ble_mesh_provisioner_set_static_oob_value(BMT_MESH_STATIC_OOB_VAL, sizeof(BMT_MESH_STATIC_OOB_VAL));
	if (err != ESP_OK)
		return err;

	err = esp_ble_mesh_provisioner_set_dev_uuid_match(NULL, 0, 0, false);
	if (err != ESP_OK)
		return err;
	err = esp_ble_mesh_provisioner_prov_enable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
	if (err != ESP_OK)
		return err;

	const uint8_t* exist_net = esp_ble_mesh_provisioner_get_local_net_key(s_net_key_idx);
	if (!exist_net)
	{
		err = esp_ble_mesh_provisioner_add_local_net_key(s_net_key, s_net_key_idx);
		if (err != ESP_OK)
			return err;
		ESP_LOGI(TAG, "NetKey added");
	}
	else
	{
		memcpy(s_net_key, exist_net, sizeof(s_net_key));
		ESP_LOGI(TAG, "NetKey already exists in stack (SETTINGS restore/auto-create)");
	}

	const uint8_t* exist_app = esp_ble_mesh_provisioner_get_local_app_key(s_net_key_idx, s_app_key_idx);
	if (!exist_app)
	{
		err = esp_ble_mesh_provisioner_add_local_app_key(s_app_key, s_net_key_idx, s_app_key_idx);
		if (err != ESP_OK)
			return err;
		ESP_LOGI(TAG, "AppKey added");
	}
	else
	{
		memcpy(s_app_key, exist_app, sizeof(s_app_key));
		ESP_LOGI(TAG, "AppKey already exists in stack (SETTINGS restore)");
	}

	err = esp_ble_mesh_provisioner_bind_app_key_to_local_model(
	    0x0001, s_app_key_idx, BMT_VND_MODEL_ID, BMT_CID_ESP);
	if (err == ESP_OK)
	{
		ESP_LOGI(TAG, "AppKey bound to local vendor model: 0x%04x", s_app_key_idx);
	}
	else if (err == ESP_ERR_INVALID_STATE)
	{
		ESP_LOGI(TAG, "AppKey already bound (restored from NVS) — OK");
	}
	else
	{
		ESP_LOGE(TAG, "bind_app_key_to_local_model FAILED: %s", esp_err_to_name(err));
		return err;
	}

	ESP_LOGI(TAG, "BLE Mesh Gateway init OK");
	return ESP_OK;
}

void bmt_mesh_start_node_ping(void)
{
	xTaskCreate(node_ping_task, "bmt_node_ping", 4096, NULL, 3, NULL);
}

esp_err_t bmt_mesh_publish(uint16_t dst, uint32_t opcode, const void* data, uint16_t len)
{
	s_vnd_models[0].pub->publish_addr = dst;
	s_vnd_models[0].pub->app_idx = s_app_key_idx;
	s_vnd_models[0].pub->ttl = 7;
	esp_err_t err = esp_ble_mesh_model_publish(
	    &s_vnd_models[0], opcode, len, (uint8_t*)data, ROLE_PROVISIONER);
	if (err != ESP_OK)
		ESP_LOGW(TAG, "publish 0x%08" PRIx32 " -> 0x%04x FAILED: %s", opcode, dst, esp_err_to_name(err));
	return err;
}

uint32_t bmt_mesh_get_received_count(void)
{
	return s_mesh_received;
}

int bmt_mesh_wipe_all_provisioned(void)
{
	const esp_ble_mesh_node_t** entry = esp_ble_mesh_provisioner_get_node_table_entry();
	int erased = 0;
	if (entry)
	{
		for (int i = 0; i < CONFIG_BLE_MESH_MAX_PROV_NODES; i++)
			if (entry[i])
			{
				esp_ble_mesh_provisioner_delete_node_with_uuid(entry[i]->dev_uuid);
				erased++;
			}
	}
	return erased;
}

void bmt_mesh_print_keys(void)
{
#ifdef BMT_DEBUG_PRINT_KEYS
	printf("NetKey: ");
	for (int i = 0; i < 16; i++)
	{
		printf("%02X", s_net_key[i]);
		if (i != 15)
			printf(":");
	}
	printf("\n");
	printf("AppKey: ");
	for (int i = 0; i < 16; i++)
	{
		printf("%02X", s_app_key[i]);
		if (i != 15)
			printf(":");
	}
	printf("\n");
#else
	printf("NetKey/AppKey: [an — enable BMT_DEBUG_PRINT_KEYS de in ra hex]\n");
#endif
}
