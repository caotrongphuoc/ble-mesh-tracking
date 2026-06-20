/* ============================================================================
 * BMT (BLE Mesh Tracking) — RELAY NODE firmware
 * ----------------------------------------------------------------------------
 * Role  : Passive forwarder (mở rộng vùng phủ sóng BLE Mesh)
 *         Không có vendor model, không parse data tag
 *         Mesh stack tự forward khi relay = ENABLED
 * Board : ESP32 DevKitC WROOM-32
 * IDF   : v6.0
 *
 * Models : Config Server (bắt buộc) + Health Server (để gateway monitor)
 *
 * UUID prefix : "RELAY" (0x52,0x45,0x4C,0x41,0x59) — Gateway dùng
 *               bmt_uuid_is_relay() để phân biệt với SCAN node.
 *               Byte cuối UUID = relay ID, đổi cho từng relay (0x01, 0x02...)
 * ============================================================================ */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "driver/uart.h"

#include "esp_bt.h"
#include "esp_bt_main.h"

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_health_model_api.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define TAG                             "BMT_RELAY"

/* ============================================================================
 * USER CONFIG — đổi cho từng relay
 * ============================================================================ */
#define BMT_CID_ESP                     0x02E5
#define BMT_RELAY_MONITOR_INTERVAL_MS   30000
#define BMT_PROV_COMPLETE_BIT           BIT0

/* ============================================================================
 * BLE MESH NODE STATE
 * ============================================================================ */
static uint8_t g_bmt_relay_uuid[16] = {
    0x52, 0x45, 0x4C, 0x41, 0x59, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02
    /* ^^^ byte cuối = relay ID, đổi cho từng relay (0x01, 0x02, 0x03...) */
};

static uint16_t g_bmt_node_addr = 0x0000;
static uint16_t g_bmt_net_idx   = 0xFFFF;
static uint16_t g_bmt_app_idx   = 0xFFFF;

static EventGroupHandle_t g_bmt_mesh_evgrp;

/* ============================================================================
 * BLUETOOTH INIT
 * ============================================================================ */
static esp_err_t bmt_bluetooth_init(void)
{
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret) { ESP_LOGE(TAG, "BT controller init failed"); return ret; }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) { ESP_LOGE(TAG, "BT controller enable failed"); return ret; }

    esp_bluedroid_config_t cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ret = esp_bluedroid_init_with_cfg(&cfg);
    if (ret) { ESP_LOGE(TAG, "Bluedroid init failed"); return ret; }

    ret = esp_bluedroid_enable();
    if (ret) { ESP_LOGE(TAG, "Bluedroid enable failed"); return ret; }

    return ESP_OK;
}

/* ============================================================================
 * MESH MODELS
 * ============================================================================ */
static const uint8_t bmt_health_test_ids[] = { ESP_BLE_MESH_HEALTH_STANDARD_TEST };

static esp_ble_mesh_health_srv_t bmt_health_server = {
    .health_test = {
        .id_count   = 1,
        .test_ids   = bmt_health_test_ids,
        .company_id = BMT_CID_ESP,
    },
};

ESP_BLE_MESH_HEALTH_PUB_DEFINE(bmt_health_pub, 0, ROLE_NODE);

static esp_ble_mesh_cfg_srv_t bmt_cfg_server = {
    /* MAX retransmit cho Relay — đây là vai trò chính: forward PDU mạnh mẽ
     * net_transmit (7,10)     : own messages transmit 8 lần, cách 10ms
     * relay_retransmit (7,10) : forward PDU người khác 8 lần
     * Trade-off: air time tăng nhưng Relay đóng vai trò khuếch đại tín hiệu */
    .net_transmit     = ESP_BLE_MESH_TRANSMIT(7, 10),
    .relay            = ESP_BLE_MESH_RELAY_ENABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(7, 10),
    .beacon           = ESP_BLE_MESH_BEACON_ENABLED,
    .gatt_proxy       = ESP_BLE_MESH_GATT_PROXY_ENABLED,
    /* Friend node: buffer message cho low-power node, tăng reliability */
    .friend_state     = ESP_BLE_MESH_FRIEND_ENABLED,
    .default_ttl      = 7,
};

static esp_ble_mesh_model_t bmt_root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&bmt_cfg_server),
    ESP_BLE_MESH_MODEL_HEALTH_SRV(&bmt_health_server, &bmt_health_pub),
};

static esp_ble_mesh_elem_t bmt_elements[] = {
    ESP_BLE_MESH_ELEMENT(0, bmt_root_models, ESP_BLE_MESH_MODEL_NONE),
};

static esp_ble_mesh_comp_t bmt_composition = {
    .cid           = BMT_CID_ESP,
    .element_count = ARRAY_SIZE(bmt_elements),
    .elements      = bmt_elements,
};

static esp_ble_mesh_prov_t bmt_provision = { .uuid = g_bmt_relay_uuid };

/* ============================================================================
 * MESH CALLBACKS
 * ============================================================================ */
static void bmt_mesh_prov_cb(esp_ble_mesh_prov_cb_event_t event,
                             esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT:
        g_bmt_net_idx   = param->node_prov_complete.net_idx;
        g_bmt_node_addr = param->node_prov_complete.addr;
        ESP_LOGI(TAG, "Provision complete! addr=0x%04x net_idx=0x%04x",
                 g_bmt_node_addr, g_bmt_net_idx);
        xEventGroupSetBits(g_bmt_mesh_evgrp, BMT_PROV_COMPLETE_BIT);
        break;

    case ESP_BLE_MESH_NODE_PROV_RESET_EVT:
        ESP_LOGI(TAG, "Node reset -> unprovisioned");
        g_bmt_node_addr = 0x0000;
        g_bmt_net_idx   = 0xFFFF;
        g_bmt_app_idx   = 0xFFFF;
        xEventGroupClearBits(g_bmt_mesh_evgrp, BMT_PROV_COMPLETE_BIT);
        esp_ble_mesh_node_prov_enable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
        break;

    default: break;
    }
}

static void bmt_mesh_cfg_server_cb(esp_ble_mesh_cfg_server_cb_event_t event,
                                   esp_ble_mesh_cfg_server_cb_param_t *param)
{
    if (event != ESP_BLE_MESH_CFG_SERVER_STATE_CHANGE_EVT) return;

    switch (param->ctx.recv_op) {
    case ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD:
        /* Relay nhận AppKey từ gateway nhưng không cần dùng để publish
         * (relay không có vendor model). Mesh stack vẫn forward gói data
         * ở tầng network bất kể có AppKey hay không. */
        g_bmt_app_idx = param->value.state_change.appkey_add.app_idx;
        ESP_LOGI(TAG, "AppKey received idx=0x%04x (relay không cần để forward)",
                 g_bmt_app_idx);
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
}

static void bmt_mesh_health_server_cb(esp_ble_mesh_health_server_cb_event_t event,
                                      esp_ble_mesh_health_server_cb_param_t *param)
{
    (void)param;
    ESP_LOGI(TAG, "Health event: %d", event);
}

/* ============================================================================
 * MESH INIT
 *
 * Sau reboot:
 *   - Nếu NVS còn lưu provision → restore ngay, set PROV_COMPLETE_BIT
 *     để monitor task chạy luôn (PROV_COMPLETE_EVT KHÔNG trigger lại).
 *   - Nếu chưa provision → enable advertising beacon, đợi gateway.
 * ============================================================================ */
static esp_err_t bmt_ble_mesh_init_relay(void)
{
    esp_ble_mesh_register_prov_callback(bmt_mesh_prov_cb);
    esp_ble_mesh_register_config_server_callback(bmt_mesh_cfg_server_cb);
    esp_ble_mesh_register_health_server_callback(bmt_mesh_health_server_cb);

    ESP_ERROR_CHECK(esp_ble_mesh_init(&bmt_provision, &bmt_composition));

    bool provisioned = esp_ble_mesh_node_is_provisioned();

    if (provisioned) {
        /* Đã provision (NVS còn lưu).
         * g_bmt_node_addr chỉ dùng để check != 0x0000 — set placeholder 0x0001.
         * AppKey & model binding đã được mesh stack tự restore. */
        ESP_LOGI(TAG, "Already provisioned (restored from NVS)");
        g_bmt_node_addr = 0x0001;
        g_bmt_app_idx   = 0x0000;
        xEventGroupSetBits(g_bmt_mesh_evgrp, BMT_PROV_COMPLETE_BIT);
    } else {
        ESP_ERROR_CHECK(esp_ble_mesh_node_prov_enable(
            ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT));
        ESP_LOGI(TAG, "BLE Mesh Relay initialized");
        ESP_LOGI(TAG, "UUID: RELAY (52:45:4C:41:59:...:%02X)", g_bmt_relay_uuid[15]);
        ESP_LOGI(TAG, "Waiting for Gateway to provision...");
    }

    return ESP_OK;
}

/* ============================================================================
 * HELPERS
 * ============================================================================ */
static void bmt_print_hex_line(const char *label, const uint8_t *data, int len)
{
    printf("%s", label);
    for (int i = 0; i < len; i++) {
        printf("%02X", data[i]);
        if (i != len - 1) printf(":");
    }
    printf("\n");
}

static void bmt_print_relay_status(const char *title)
{
    printf("\n========== %s ==========\n", title);
    bmt_print_hex_line("UUID      : ", g_bmt_relay_uuid, 16);
    printf("Node addr : 0x%04X %s\n", g_bmt_node_addr,
           g_bmt_node_addr == 0x0000 ? "(UNPROVISIONED)" : "(PROVISIONED & RELAYING)");
    printf("Net idx   : 0x%04X\n", g_bmt_net_idx);
    printf("App idx   : 0x%04X %s\n", g_bmt_app_idx,
           g_bmt_app_idx == 0xFFFF ? "(no AppKey — OK for relay)" : "(AppKey OK)");
    printf("Relay     : %s\n",
           bmt_cfg_server.relay == ESP_BLE_MESH_RELAY_ENABLED ? "ENABLED" : "DISABLED");
    printf("Retransmit: count=%d interval=%dms\n",
           ESP_BLE_MESH_GET_TRANSMIT_COUNT(bmt_cfg_server.relay_retransmit),
           ESP_BLE_MESH_GET_TRANSMIT_INTERVAL(bmt_cfg_server.relay_retransmit));
    printf("TTL       : %u\n", bmt_cfg_server.default_ttl);
    printf("========================================\n");
}

/* ============================================================================
 * MONITOR TASK
 * In status định kỳ để theo dõi trên serial monitor
 * ============================================================================ */
static void bmt_relay_monitor_task(void *arg)
{
    (void)arg;

    /* Đợi mesh init xong + 5s ổn định */
    xEventGroupWaitBits(g_bmt_mesh_evgrp, BMT_PROV_COMPLETE_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(5000));

    bmt_print_relay_status("RELAY READY");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(BMT_RELAY_MONITOR_INTERVAL_MS));

        uint32_t uptime_s = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
        uint32_t h = uptime_s / 3600;
        uint32_t m = (uptime_s % 3600) / 60;
        uint32_t s = uptime_s % 60;

        printf("\n========== RELAY HEALTH ==========\n");
        printf("Status    : %s\n",
               g_bmt_node_addr != 0x0000 ? "PROVISIONED & RELAYING" : "UNPROVISIONED");
        printf("Node addr : 0x%04X\n", g_bmt_node_addr);
        printf("Uptime    : %02" PRIu32 "h %02" PRIu32 "m %02" PRIu32 "s\n", h, m, s);
        printf("Free heap : %lu bytes\n", (unsigned long)esp_get_free_heap_size());
        printf("==================================\n");
    }
}

/* ============================================================================
 * UART COMMAND TASK
 *   r → Reset mesh provision (xóa NVS, về unprovisioned)
 *   1 → In trạng thái hiện tại
 * ============================================================================ */
static void bmt_uart_cmd_task(void *arg)
{
    (void)arg;

    uart_config_t uart_cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_0, &uart_cfg);

    printf("\n===== BMT RELAY COMMANDS =====\n");
    printf("r -> RESET mesh (xoa NVS, ve unprovisioned)\n");
    printf("1 -> STATUS hien tai\n");
    printf("==============================\n");

    uint8_t ch;
    while (1) {
        int len = uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(200));
        if (len <= 0 || ch == '\r' || ch == '\n') continue;

        switch (ch) {
        case 'r':
        case 'R':
            printf("\n[UART] Resetting mesh provision...\n");
            printf("[UART] Relay se ve unprovisioned va broadcast beacon moi\n");
            printf("[UART] Gateway can scan lai de provision\n\n");
            esp_ble_mesh_node_local_reset();
            g_bmt_node_addr = 0x0000;
            g_bmt_net_idx   = 0xFFFF;
            g_bmt_app_idx   = 0xFFFF;
            break;

        case '1':
            bmt_print_relay_status("RELAY STATUS");
            break;

        default:
            printf("[UART] Unknown: %c  (r=reset, 1=status)\n", ch);
            break;
        }
    }
}

/* ============================================================================
 * MAIN
 * ============================================================================ */
void app_main(void)
{
    ESP_LOGI(TAG, "=== BMT Relay Node Starting ===");
    ESP_LOGI(TAG, "Relay ID: 0x%02X", g_bmt_relay_uuid[15]);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    g_bmt_mesh_evgrp = xEventGroupCreate();

    err = bmt_bluetooth_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BT init failed: %s", esp_err_to_name(err)); return;
    }

    /* MAX TX POWER +9 dBm — Relay đóng vai trò forward, cần TX mạnh */
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV,     ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN,    ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    ESP_LOGI(TAG, "BLE TX power set to +9 dBm (max for ESP32 classic)");

    err = bmt_ble_mesh_init_relay();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Mesh init failed: %s", esp_err_to_name(err)); return;
    }

    xTaskCreate(bmt_relay_monitor_task, "bmt_relay_mon", 2048, NULL, 3, NULL);
    xTaskCreate(bmt_uart_cmd_task,      "bmt_uart",      2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "=== BMT Relay READY (r=reset, 1=status) ===");
}