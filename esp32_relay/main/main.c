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

#define TAG "RELAY"

/* ==========================================================================
 * RELAY NODE - PASSIVE FORWARDER
 *
 * Vai trò trong hệ thống:
 *   - Gateway (provisioner, addr 0x0001) ←→ Scanner (vendor model publish)
 *   - Relay = forwarder tầng mesh → mở rộng vùng phủ sóng
 *   - Relay KHÔNG có vendor model, KHÔNG parse data tag
 *     mesh stack tự forward gói khi relay = ENABLED
 *
 * Models trên relay:
 *   - Config Server (bắt buộc)
 *   - Health Server (để gateway monitor)
 *
 * UUID prefix "RELAY" (0x52,0x45,0x4C,0x41,0x59) — gateway dùng
 * uuid_is_relay() để phân biệt với SCANNER khi auto-provision.
 *
 * Mỗi relay phải có UUID byte cuối khác nhau:
 *   Relay 1: 0x01, Relay 2: 0x02, Relay 3: 0x03, ...
 * ========================================================================== */

#define RELAY_MONITOR_INTERVAL_MS   30000   /* In status mỗi 30s */
#define PROV_COMPLETE_BIT           BIT0

/* ==========================================================================
 * BLE MESH NODE STATE
 * ========================================================================== */
static uint8_t dev_uuid[16] = {
    0x52, 0x45, 0x4C, 0x41, 0x59, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02
    /* ^^^ đổi byte cuối thành 0x02, 0x03... cho relay tiếp theo */
};

static uint16_t g_node_addr = 0x0000;
static uint16_t g_net_idx   = 0xFFFF;
static uint16_t g_app_idx   = 0xFFFF;

static EventGroupHandle_t g_mesh_event_group;

/* ==========================================================================
 * BLUETOOTH CONTROLLER + BLUEDROID HOST INIT
 * Inline thay cho bluetooth_init() trong example_init component để
 * không phải kéo dependency từ ESP-IDF examples folder.
 * ========================================================================== */
static esp_err_t bluetooth_init(void)
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

/* ==========================================================================
 * BLE MESH MODELS
 * Composition: 1 element gồm Config Server + Health Server
 * ========================================================================== */
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

static esp_ble_mesh_prov_t provision = { .uuid = dev_uuid };

/* ==========================================================================
 * BLE MESH CALLBACKS
 * ========================================================================== */
static void mesh_prov_cb(esp_ble_mesh_prov_cb_event_t event,
                         esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT:
        g_net_idx   = param->node_prov_complete.net_idx;
        g_node_addr = param->node_prov_complete.addr;
        ESP_LOGI(TAG, "Provision complete! addr=0x%04x net_idx=0x%04x",
                 g_node_addr, g_net_idx);
        xEventGroupSetBits(g_mesh_event_group, PROV_COMPLETE_BIT);
        break;

    case ESP_BLE_MESH_NODE_PROV_RESET_EVT:
        ESP_LOGI(TAG, "Node reset -> unprovisioned");
        g_node_addr = 0x0000; g_net_idx = 0xFFFF; g_app_idx = 0xFFFF;
        xEventGroupClearBits(g_mesh_event_group, PROV_COMPLETE_BIT);
        esp_ble_mesh_node_prov_enable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
        break;

    default:
        break;
    }
}

static void mesh_config_server_cb(esp_ble_mesh_cfg_server_cb_event_t event,
                                  esp_ble_mesh_cfg_server_cb_param_t *param)
{
    if (event != ESP_BLE_MESH_CFG_SERVER_STATE_CHANGE_EVT) return;

    switch (param->ctx.recv_op) {
    case ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD:
        /* Relay nhận AppKey từ gateway — lưu lại nhưng không cần dùng
         * để publish (relay không có vendor model). Mesh stack vẫn
         * forward gói data ở tầng network bất kể có AppKey hay không. */
        g_app_idx = param->value.state_change.appkey_add.app_idx;
        ESP_LOGI(TAG, "AppKey received idx=0x%04x (relay không cần để forward)",
                 g_app_idx);
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

static void mesh_health_server_cb(esp_ble_mesh_health_server_cb_event_t event,
                                  esp_ble_mesh_health_server_cb_param_t *param)
{
    (void)param;
    ESP_LOGI(TAG, "Health event: %d", event);
}

/* ==========================================================================
 * BLE MESH INIT
 *
 * Sau reboot:
 *   - Nếu NVS còn lưu provision → restore ngay, set PROV_COMPLETE_BIT
 *     để monitor task chạy luôn (PROV_COMPLETE_EVT KHÔNG trigger lại).
 *   - Nếu chưa provision → enable advertising beacon, đợi gateway.
 * ========================================================================== */
static esp_err_t ble_mesh_init_relay(void)
{
    esp_ble_mesh_register_prov_callback(mesh_prov_cb);
    esp_ble_mesh_register_config_server_callback(mesh_config_server_cb);
    esp_ble_mesh_register_health_server_callback(mesh_health_server_cb);

    ESP_ERROR_CHECK(esp_ble_mesh_init(&provision, &composition));

    bool provisioned = esp_ble_mesh_node_is_provisioned();

    if (provisioned) {
        /* Đã provision (NVS còn lưu). g_node_addr chỉ dùng để check
         * != 0x0000 trong status print — set placeholder 0x0001 là đủ.
         * AppKey & model binding đã được mesh stack tự restore. */
        ESP_LOGI(TAG, "Already provisioned (restored from NVS)");
        g_node_addr = 0x0001;
        g_app_idx   = 0x0000;
        xEventGroupSetBits(g_mesh_event_group, PROV_COMPLETE_BIT);
    } else {
        ESP_ERROR_CHECK(esp_ble_mesh_node_prov_enable(
            ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT));
        ESP_LOGI(TAG, "BLE Mesh Relay initialized");
        ESP_LOGI(TAG, "UUID: RELAY (52:45:4C:41:59:...:%02X)", dev_uuid[15]);
        ESP_LOGI(TAG, "Waiting for Gateway to provision...");
    }

    return ESP_OK;
}

/* ==========================================================================
 * HELPERS
 * ========================================================================== */
static void print_hex_line(const char *label, const uint8_t *data, int len)
{
    printf("%s", label);
    for (int i = 0; i < len; i++) {
        printf("%02X", data[i]);
        if (i != len - 1) printf(":");
    }
    printf("\n");
}

static void print_relay_status(const char *title)
{
    printf("\n========== %s ==========\n", title);
    print_hex_line("UUID      : ", dev_uuid, 16);
    printf("Node addr : 0x%04X %s\n", g_node_addr,
           g_node_addr == 0x0000 ? "(UNPROVISIONED)" : "(PROVISIONED & RELAYING)");
    printf("Net idx   : 0x%04X\n", g_net_idx);
    printf("App idx   : 0x%04X %s\n", g_app_idx,
           g_app_idx == 0xFFFF ? "(no AppKey — OK for relay)" : "(AppKey OK)");
    printf("Relay     : %s\n",
           config_server.relay == ESP_BLE_MESH_RELAY_ENABLED ? "ENABLED" : "DISABLED");
    printf("Retransmit: count=%d interval=%dms\n",
           ESP_BLE_MESH_GET_TRANSMIT_COUNT(config_server.relay_retransmit),
           ESP_BLE_MESH_GET_TRANSMIT_INTERVAL(config_server.relay_retransmit));
    printf("TTL       : %u\n", config_server.default_ttl);
    printf("========================================\n");
}

/* ==========================================================================
 * MONITOR TASK
 * In status định kỳ để theo dõi trên MobaXterm
 * ========================================================================== */
static void relay_monitor_task(void *arg)
{
    (void)arg;

    /* Đợi mesh init xong + 5s ổn định */
    xEventGroupWaitBits(g_mesh_event_group, PROV_COMPLETE_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(5000));

    print_relay_status("RELAY READY");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(RELAY_MONITOR_INTERVAL_MS));

        uint32_t uptime_s = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
        uint32_t h = uptime_s / 3600;
        uint32_t m = (uptime_s % 3600) / 60;
        uint32_t s = uptime_s % 60;

        printf("\n========== RELAY HEALTH ==========\n");
        printf("Status    : %s\n",
               g_node_addr != 0x0000 ? "PROVISIONED & RELAYING" : "UNPROVISIONED");
        printf("Node addr : 0x%04X\n", g_node_addr);
        printf("Uptime    : %02" PRIu32 "h %02" PRIu32 "m %02" PRIu32 "s\n", h, m, s);
        printf("Free heap : %lu bytes\n", (unsigned long)esp_get_free_heap_size());
        printf("==================================\n");
    }
}

/* ==========================================================================
 * UART COMMAND TASK
 * Lệnh debug qua serial (MobaXterm):
 *   r → Reset mesh provision (xóa NVS, về unprovisioned)
 *       Dùng khi Gateway bị clear/reflash mà relay vẫn còn NVS cũ
 *   1 → In trạng thái hiện tại
 * ========================================================================== */
static void uart_cmd_task(void *arg)
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

    printf("\n===== RELAY UART COMMANDS =====\n");
    printf("r -> RESET mesh (xoa NVS, ve unprovisioned)\n");
    printf("1 -> STATUS hien tai\n");
    printf("===============================\n");

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
            g_node_addr = 0x0000;
            g_net_idx   = 0xFFFF;
            g_app_idx   = 0xFFFF;
            break;

        case '1':
            print_relay_status("RELAY STATUS");
            break;

        default:
            printf("[UART] Unknown: %c  (r=reset, 1=status)\n", ch);
            break;
        }
    }
}

/* ==========================================================================
 * MAIN
 * ========================================================================== */
void app_main(void)
{
    ESP_LOGI(TAG, "=== BLE Mesh Relay Node Starting ===");
    ESP_LOGI(TAG, "Relay ID: 0x%02X", dev_uuid[15]);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    g_mesh_event_group = xEventGroupCreate();

    err = bluetooth_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "BT init failed: %s", esp_err_to_name(err)); return; }

    err = ble_mesh_init_relay();
    if (err != ESP_OK) { ESP_LOGE(TAG, "Mesh init failed: %s", esp_err_to_name(err)); return; }

    xTaskCreate(relay_monitor_task, "relay_monitor", 2048, NULL, 3, NULL);
    xTaskCreate(uart_cmd_task,      "uart_cmd",      2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "=== RELAY READY (r=reset mesh, 1=status) ===");
}