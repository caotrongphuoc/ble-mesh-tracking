#include "bmt_mesh.h"

#include "esp_log.h"

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_health_model_api.h"

#include "bmt_types.h"
#include "bmt_config.h"
#include "bmt_ota.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

static const char* TAG = "BMT_MESH";

static const uint8_t s_health_test_ids[] = {ESP_BLE_MESH_HEALTH_STANDARD_TEST};

static esp_ble_mesh_health_srv_t s_health_server = {
    .health_test = {
        .id_count = 1,
        .test_ids = s_health_test_ids,
        .company_id = BMT_CID_ESPRESSIF,
    },
};

ESP_BLE_MESH_HEALTH_PUB_DEFINE(s_health_pub, 0, ROLE_NODE);

static esp_ble_mesh_cfg_srv_t s_cfg_server = BMT_MESH_NODE_CFG_SRV_INIT;

/* Vendor server model - receives RESET_CMD / OTA_TRIGGER and
 * publishes OTA_RESULT. */
ESP_BLE_MESH_MODEL_PUB_DEFINE(s_vnd_pub, sizeof(bmt_ota_result_t) + 4, ROLE_NODE);

static esp_ble_mesh_model_op_t s_vnd_ops[] = {
    ESP_BLE_MESH_MODEL_OP(BMT_OP_VND_RESET_CMD, 1),
    ESP_BLE_MESH_MODEL_OP(BMT_OP_VND_OTA_TRIGGER, 1),
    ESP_BLE_MESH_MODEL_OP(BMT_OP_VND_OTA_RESULT, sizeof(bmt_ota_result_t)),
    ESP_BLE_MESH_MODEL_OP_END,
};

static esp_ble_mesh_model_t s_vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(BMT_CID_ESPRESSIF, BMT_VND_MODEL_ID,
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
    .cid = BMT_CID_ESPRESSIF,
    .element_count = ARRAY_SIZE(s_elements),
    .elements = s_elements,
};
static const uint8_t BMT_MESH_STATIC_OOB_VAL[16] = BMT_MESH_STATIC_OOB_INIT;

static esp_ble_mesh_prov_t s_provision = {
    .uuid = BMT_RELAY_UUID,
    .static_val = BMT_MESH_STATIC_OOB_VAL,
    .static_val_len = sizeof(BMT_MESH_STATIC_OOB_VAL),
};

static void mesh_health_server_cb(esp_ble_mesh_health_server_cb_event_t event,
                                  esp_ble_mesh_health_server_cb_param_t* param)
{
	(void)param;
	ESP_LOGI(TAG, "Health event: %d", event);
}

static void on_vnd_msg(uint32_t opcode, uint16_t src, const uint8_t* msg, uint16_t len)
{
	(void)msg;
	(void)len;
	if (opcode == BMT_OP_VND_OTA_TRIGGER)
	{
		ESP_LOGW(TAG, "[OTA] OTA_TRIGGER from 0x%04x - starting WiFi OTA", src);
		bmt_ota_trigger();
	}
}

esp_err_t bmt_mesh_init(void)
{
	esp_ble_mesh_register_health_server_callback(mesh_health_server_cb);

	esp_err_t err = bmt_mesh_node_start(&s_provision, &s_composition, &s_vnd_models[0], on_vnd_msg);
	if (err != ESP_OK)
		return err;

	if (!bmt_mesh_is_provisioned())
	{
		ESP_LOGI(TAG, "BLE Mesh Relay initialized");
		ESP_LOGI(TAG, "UUID: RELAY ...:%02X | Waiting provision...", BMT_RELAY_UUID[15]);
	}
	return ESP_OK;
}

bool bmt_mesh_relay_enabled(void)
{
	return s_cfg_server.relay == ESP_BLE_MESH_RELAY_ENABLED;
}
