#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_health_model_api.h"

#include "ble_mesh_example_init.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define TAG "RELAY_NODE"

/* ========================= CONFIG =========================
 * QUAN TRỌNG: Mỗi relay phải có UUID byte cuối khác nhau
 *   Relay 1: 0x01 (mặc định)
 *   Relay 2: đổi thành 0x02
 *   Relay 3: đổi thành 0x03 ...
 * ========================================================= */
#define RELAY_MONITOR_INTERVAL_MS   30000   /* In health status mỗi 30s */

static uint8_t dev_uuid[16] = {
    0x52, 0x45, 0x4C, 0x41, 0x59, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02
    /* ^^^ đổi byte cuối thành 0x02, 0x03... cho relay tiếp theo */
};

static uint16_t g_node_addr = 0x0000;
static uint16_t g_net_idx   = 0xFFFF;
static uint16_t g_app_idx   = 0xFFFF;
static uint32_t g_uptime_s  = 0;

/* ========================= MCU TEMP ========================= */
#ifdef __cplusplus
extern "C" {
#endif
uint8_t temprature_sens_read(void);
#ifdef __cplusplus
}
#endif

static float read_mcu_temp(void)
{
    uint8_t raw = temprature_sens_read();
    return (raw - 32) / 1.8f;
}

/* ========================= BLE MESH MODELS ========================= */
static const uint8_t health_test_ids[] = { ESP_BLE_MESH_HEALTH_STANDARD_TEST };

static esp_ble_mesh_health_srv_t health_server = {
    .health_test = {
        .id_count   = 1,
        .test_ids   = health_test_ids,
        .company_id = 0x02E5,
    },
};

ESP_BLE_MESH_HEALTH_PUB_DEFINE(health_pub, 0, ROLE_NODE);

static esp_ble_mesh_cfg_srv_t config_server = {
    .net_transmit     = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay            = ESP_BLE_MESH_RELAY_ENABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon           = ESP_BLE_MESH_BEACON_ENABLED,
    .gatt_proxy       = ESP_BLE_MESH_GATT_PROXY_ENABLED,
    .friend_state     = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .default_ttl      = 7,
};

static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
    ESP_BLE_MESH_MODEL_HEALTH_SRV(&health_server, &health_pub),
};

static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models, ESP_BLE_MESH_MODEL_NONE),
};

static esp_ble_mesh_comp_t composition = {
    .cid           = 0x02E5,
    .element_count = ARRAY_SIZE(elements),
    .elements      = elements,
};

static esp_ble_mesh_prov_t provision = {
    .uuid = dev_uuid,
};

/* ========================= HELPERS ========================= */
static void print_hex_line(const char *label, const uint8_t *data, int len)
{
    printf("%s", label);
    for (int i = 0; i < len; i++) {
        printf("%02X", data[i]);
        if (i != len - 1) printf(":");
    }
    printf("\n");
}

static void print_relay_info(const char *title)
{
    printf("\n============= %s =============\n", title);
    print_hex_line("UUID       : ", dev_uuid, 16);
    printf("Node Addr  : 0x%04X\n", g_node_addr);
    printf("NetIdx     : 0x%04X\n", g_net_idx);
    printf("AppIdx     : 0x%04X  (0xFFFF = normal, Relay (no Appkey used))\n", g_app_idx);
    printf("Relay      : ENABLED\n");
    printf("TTL        : %u\n", config_server.default_ttl);
    printf("===========================================\n");
}

/* ========================= MONITOR TASK =========================
 * In ra console mỗi 30s để nhìn trên MobaXterm:
 *  - Trạng thái provisioned/chưa
 *  - MCU temperature (cảnh báo nếu > 70°C)
 *  - Uptime
 *  - Relay config
 * ============================================================== */
static void relay_monitor_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(5000)); /* đợi system ổn định */

    while (1) {
        g_uptime_s += RELAY_MONITOR_INTERVAL_MS / 1000;

        float    mcu_temp = read_mcu_temp();
        uint32_t uptime_h = g_uptime_s / 3600;
        uint32_t uptime_m = (g_uptime_s % 3600) / 60;
        uint32_t uptime_s = g_uptime_s % 60;

        printf("\n========== RELAY HEALTH STATUS ==========\n");
        print_hex_line("UUID      : ", dev_uuid, 16);
        printf("Node Addr : 0x%04X\n", g_node_addr);
        printf("Status    : %s\n",
               g_node_addr != 0x0000 ? "PROVISIONED & RELAYING" : "UNPROVISIONED");
        printf("NetIdx    : 0x%04X\n", g_net_idx);
        printf("AppIdx    : 0x%04X\n", g_app_idx);
        printf("Relay     : %s\n",
               config_server.relay == ESP_BLE_MESH_RELAY_ENABLED ? "ENABLED" : "DISABLED");
        printf("Retransmit: count=%d interval=%dms\n",
               ESP_BLE_MESH_GET_TRANSMIT_COUNT(config_server.relay_retransmit),
               ESP_BLE_MESH_GET_TRANSMIT_INTERVAL(config_server.relay_retransmit));
        printf("TTL       : %u\n", config_server.default_ttl);
        printf("MCU Temp  : %.1f C%s\n", mcu_temp,
               mcu_temp > 70.0f ? "  *** HIGH TEMP WARNING ***" : "");
        printf("Uptime    : %02" PRIu32 "h %02" PRIu32 "m %02" PRIu32 "s\n",
               uptime_h, uptime_m, uptime_s);
        printf("=========================================\n");

        vTaskDelay(pdMS_TO_TICKS(RELAY_MONITOR_INTERVAL_MS));
    }
}

/* ========================= CALLBACKS ========================= */
static void prov_complete(uint16_t net_idx, uint16_t addr,
                          uint8_t flags, uint32_t iv_index)
{
    (void)flags;
    (void)iv_index;
    g_net_idx   = net_idx;
    g_node_addr = addr;
    ESP_LOGI(TAG, "Provisioning complete! addr=0x%04x", addr);
    print_relay_info("RELAY NODE PROVISIONED");
}

static void mesh_prov_cb(esp_ble_mesh_prov_cb_event_t event,
                         esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT:
        prov_complete(param->node_prov_complete.net_idx,
                      param->node_prov_complete.addr,
                      param->node_prov_complete.flags,
                      param->node_prov_complete.iv_index);
        break;

    case ESP_BLE_MESH_NODE_PROV_RESET_EVT:
        ESP_LOGI(TAG, "Node reset -> back to unprovisioned");
        g_node_addr = 0x0000;
        g_net_idx   = 0xFFFF;
        g_app_idx   = 0xFFFF;
        ESP_ERROR_CHECK(esp_ble_mesh_node_prov_enable(
            ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT));
        print_relay_info("RELAY NODE RESET TO UNPROVISIONED");
        break;

    default:
        break;
    }
}

static void mesh_config_server_cb(esp_ble_mesh_cfg_server_cb_event_t event,
                                  esp_ble_mesh_cfg_server_cb_param_t *param)
{
    if (event != ESP_BLE_MESH_CFG_SERVER_STATE_CHANGE_EVT) return;

    ESP_LOGI(TAG, "CFG SERVER opcode: 0x%04" PRIx32, param->ctx.recv_op);

    switch (param->ctx.recv_op) {
    case ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD:
        /* Relay nhận AppKey từ gateway — bình thường, không cần dùng */
        g_app_idx = param->value.state_change.appkey_add.app_idx;
        ESP_LOGI(TAG, "AppKey added app_idx=0x%04x (relay không cần AppKey để forward)", g_app_idx);
        break;
    case ESP_BLE_MESH_MODEL_OP_RELAY_SET:
        ESP_LOGI(TAG, "Relay state changed by config client");
        break;
    case ESP_BLE_MESH_MODEL_OP_MODEL_SUB_ADD:
        ESP_LOGI(TAG, "Model subscription added");
        break;
    case ESP_BLE_MESH_MODEL_OP_MODEL_PUB_SET:
        ESP_LOGI(TAG, "Model publication set");
        break;
    default:
        break;
    }

    print_relay_info("RELAY NODE CONFIG UPDATED");
}

static void mesh_health_server_cb(esp_ble_mesh_health_server_cb_event_t event,
                                  esp_ble_mesh_health_server_cb_param_t *param)
{
    (void)param;
    ESP_LOGI(TAG, "Health event: %d", event);
}

/* ========================= INIT ========================= */
static esp_err_t ble_mesh_init(void)
{
    esp_ble_mesh_register_prov_callback(mesh_prov_cb);
    esp_ble_mesh_register_config_server_callback(mesh_config_server_cb);
    esp_ble_mesh_register_health_server_callback(mesh_health_server_cb);

    ESP_ERROR_CHECK(esp_ble_mesh_init(&provision, &composition));
    ESP_ERROR_CHECK(esp_ble_mesh_node_prov_enable(
        ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT));

    ESP_LOGI(TAG, "BLE Mesh Relay Node initialized");
    ESP_LOGI(TAG, "Waiting for provisioning...");
    print_relay_info("RELAY NODE INITIALIZED");

    return ESP_OK;
}

/* ========================= MAIN ========================= */
void app_main(void)
{
    esp_err_t err;

    ESP_LOGI(TAG, "=== BLE Mesh Relay Node Starting ===");

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = bluetooth_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth init failed: %s", esp_err_to_name(err));
        return;
    }

    err = ble_mesh_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE Mesh init failed: %s", esp_err_to_name(err));
        return;
    }

    xTaskCreate(relay_monitor_task, "relay_monitor", 2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "=== RELAY READY ===");
}