#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "mqtt_client.h"

#include "driver/uart.h"
#include "esp_task_wdt.h"

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_local_data_operation_api.h"

#include "ble_mesh_example_init.h"

/* ==========================================================================
 * VENDOR MODEL - GATEWAY SIDE
 * Gateway nhan data tu Scanner qua Vendor Model
 * CID = 0x02E5 (Espressif)
 * ========================================================================== */
#define CID_ESP                     0x02E5
#define ESP_BLE_MESH_VND_MODEL_ID   0x0000

#define OP_VND_TAG_STATUS   ESP_BLE_MESH_MODEL_OP_3(0x00, CID_ESP)

#pragma pack(1)
/* FIX: giảm payload từ 13 xuống 8 bytes
 * BLE Mesh unsegmented max = 11 bytes (opcode 3B + data 8B)
 * 13 bytes → segmented → cần ACK từ gateway → "Ran out of retransmit"
 * 8 bytes  → unsegmented → fire-and-forget, không cần ACK
 *
 * distance: int16_t tính bằng decimeter (dm), chia 10 để ra mét
 *           max = 3276.7m, resolution = 0.1m — đủ cho indoor tracking
 * loss_pct: uint8_t 0-100%, đủ chính xác */
typedef struct {
    uint8_t  scanner_id;    /* 1B: scanner nào gửi */
    uint8_t  tag_type;      /* 1B: PERSON=0x01 / ASSET=0x02 */
    uint16_t tag_id;        /* 2B: ID của tag */
    int8_t   rssi;          /* 1B: RSSI filtered (dBm) */
    int16_t  distance_dm;   /* 2B: khoảng cách × 10 (đơn vị dm) */
    uint8_t  loss_pct;      /* 1B: loss rate % (0-100) */
} mesh_tag_report_t;        /* = 8 bytes → UNSEGMENTED ✓ */
#pragma pack()

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define TAG "GATEWAY"

/* ========================= USER CONFIG ========================= */
#define WIFI_SSID                   "TP-Link_385B"
#define WIFI_PASS                   "91324566"
#define MQTT_BROKER                 "mqtt://192.168.1.113"

#define MAX_NODES                   10

/* ========================= TIMING ========================= */
#define RELAY_PING_INTERVAL_MS      20000
#define RELAY_OFFLINE_TIMEOUT_MS    60000
#define SCAN_DURATION_MS            10000
#define GATEWAY_WDT_TIMEOUT_S       60

/* ========================= NVS ========================= */
#define NVS_NAMESPACE               "gw_mesh"
#define NVS_KEY_NODES               "node_table"

/* ========================= UART ========================= */
#define GATEWAY_UART_NUM            UART_NUM_0
#define GATEWAY_UART_BAUD           115200
#define GATEWAY_UART_RX_BUF_SIZE    2048

/* ========================= SCAN LIST ========================= */
#define MAX_SCAN_LIST               10

/* ========================= MAC CACHE ========================= */
#define MAX_MAC_CACHE               16
typedef struct {
    bool    used;
    uint8_t uuid[16];
    uint8_t mac[6];
} mac_cache_entry_t;
static mac_cache_entry_t g_mac_cache[MAX_MAC_CACHE];

static void mac_cache_store(const uint8_t *uuid, const uint8_t *mac)
{
    for (int i = 0; i < MAX_MAC_CACHE; i++) {
        if (g_mac_cache[i].used &&
            memcmp(g_mac_cache[i].uuid, uuid, 16) == 0) {
            memcpy(g_mac_cache[i].mac, mac, 6);
            return;
        }
    }
    for (int i = 0; i < MAX_MAC_CACHE; i++) {
        if (!g_mac_cache[i].used) {
            g_mac_cache[i].used = true;
            memcpy(g_mac_cache[i].uuid, uuid, 16);
            memcpy(g_mac_cache[i].mac,  mac,  6);
            return;
        }
    }
}

static bool mac_cache_get(const uint8_t *uuid, uint8_t *mac_out)
{
    for (int i = 0; i < MAX_MAC_CACHE; i++) {
        if (g_mac_cache[i].used &&
            memcmp(g_mac_cache[i].uuid, uuid, 16) == 0) {
            memcpy(mac_out, g_mac_cache[i].mac, 6);
            return true;
        }
    }
    return false;
}

/* ========================= PROVISION MODE ========================= */
typedef enum {
    PROV_MODE_AUTO   = 0,
    PROV_MODE_MANUAL = 1,
} prov_mode_t;

static prov_mode_t g_prov_mode = PROV_MODE_MANUAL;

/* ========================= KEYS ========================= */
static uint16_t net_key_idx = 0x0000;
static uint16_t app_key_idx = 0x0000;

static const uint8_t local_net_key[16] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef
};
static const uint8_t local_app_key[16] = {
    0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
    0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10
};
static uint8_t dev_uuid[ESP_BLE_MESH_OCTET16_LEN] = {
    0x47, 0x41, 0x54, 0x45, 0x57, 0x41, 0x59, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01
};

/* ========================= WIFI / MQTT ========================= */
static EventGroupHandle_t       wifi_event_group;
static const int                WIFI_CONNECTED_BIT = BIT0;
static esp_mqtt_client_handle_t mqtt_client    = NULL;
static bool                     mqtt_connected = false;

/* ========================= NODE TABLE ========================= */
typedef struct {
    bool     used;
    bool     is_relay;
    bool     is_scanner;
    bool     config_done;
    bool     online;
    uint32_t last_seen_ms;
    uint16_t addr;
    uint8_t  uuid[16];
    uint8_t  mac[6];
    char     name[32];
} node_info_t;

static node_info_t g_nodes[MAX_NODES];

/* ========================= SCAN LIST ========================= */
typedef struct {
    bool     used;
    uint8_t  uuid[16];
    uint8_t  addr[6];
    uint8_t  addr_type;
    uint16_t oob_info;
} scan_entry_t;

static scan_entry_t g_scan_list[MAX_SCAN_LIST];
static int          g_scan_count = 0;
static bool         g_scanning   = false;

/* ========================= BLE MESH ========================= */
static esp_ble_mesh_cfg_srv_t config_server = {
    .net_transmit     = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay            = ESP_BLE_MESH_RELAY_ENABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon           = ESP_BLE_MESH_BEACON_ENABLED,
    .gatt_proxy       = ESP_BLE_MESH_GATT_PROXY_ENABLED,
    .friend_state     = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .default_ttl      = 7,
};

static esp_ble_mesh_client_t config_client;

/* Vendor Model Client nhan data tu Scanner */
static esp_ble_mesh_client_t vnd_client;
static esp_ble_mesh_model_op_t vnd_ops[] = {
    ESP_BLE_MESH_MODEL_OP(OP_VND_TAG_STATUS, sizeof(mesh_tag_report_t)),
    ESP_BLE_MESH_MODEL_OP_END,
};

static esp_ble_mesh_model_t vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(CID_ESP, ESP_BLE_MESH_VND_MODEL_ID,
                              vnd_ops, NULL, &vnd_client),
};

static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
    ESP_BLE_MESH_MODEL_CFG_CLI(&config_client),
};

static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models, vnd_models),
};

static esp_ble_mesh_comp_t composition = {
    .cid           = 0x02E5,
    .element_count = ARRAY_SIZE(elements),
    .elements      = elements,
};

static esp_ble_mesh_prov_t provision = {
    .uuid               = dev_uuid,
    .prov_unicast_addr  = 0x0001,
    .prov_start_address = 0x0002,
};

/* ========================= NVS ========================= */
static void nvs_save_nodes(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, NVS_KEY_NODES, g_nodes, sizeof(g_nodes)) == ESP_OK)
        nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Node table saved to NVS");
}

static void nvs_load_nodes(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved node table, starting fresh");
        return;
    }
    if (err != ESP_OK) return;
    size_t size = sizeof(g_nodes);
    err = nvs_get_blob(h, NVS_KEY_NODES, g_nodes, &size);
    nvs_close(h);
    if (err == ESP_OK) {
        int count = 0;
        for (int i = 0; i < MAX_NODES; i++) {
            if (!g_nodes[i].used) continue;
            count++;
            if (g_nodes[i].is_relay) {
                g_nodes[i].online       = false;
                g_nodes[i].last_seen_ms = 0;
            }
        }
        ESP_LOGI(TAG, "Node table loaded (%d nodes)", count);
    }
}

static void nvs_clear_nodes(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, NVS_KEY_NODES);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "NVS cleared");
}

/* ========================= HELPERS ========================= */
static void print_hex_key(const char *label, const uint8_t *key, int len)
{
    printf("%s", label);
    for (int i = 0; i < len; i++) {
        printf("%02X", key[i]);
        if (i != len-1) printf(":");
    }
    printf("\n");
}

static void print_uuid(const uint8_t *uuid)
{
    if (!uuid) return;
    for (int i = 0; i < 16; i++) {
        printf("%02X", uuid[i]);
        if (i != 15) printf(":");
    }
}

static void print_mac(const uint8_t *mac)
{
    if (!mac) return;
    for (int i = 0; i < 6; i++) {
        printf("%02X", mac[i]);
        if (i != 5) printf(":");
    }
}

static bool uuid_is_relay(const uint8_t *uuid)
{
    if (!uuid) return false;
    return (uuid[0] == 0x52 && uuid[1] == 0x45 &&
            uuid[2] == 0x4C && uuid[3] == 0x41 && uuid[4] == 0x59);
}

static bool uuid_is_scanner(const uint8_t *uuid)
{
    if (!uuid) return false;
    return (uuid[0] == 0x53 && uuid[1] == 0x43 &&
            uuid[2] == 0x41 && uuid[3] == 0x4E);
}

static const char *uuid_type_str(const uint8_t *uuid)
{
    if (uuid_is_relay(uuid))   return "RELAY";
    if (uuid_is_scanner(uuid)) return "SCANNER";
    return "UNKNOWN";
}

static int find_node_index(uint16_t addr)
{
    for (int i = 0; i < MAX_NODES; i++)
        if (g_nodes[i].used && g_nodes[i].addr == addr) return i;
    return -1;
}

static bool uuid_already_provisioned(const uint8_t *uuid)
{
    for (int i = 0; i < MAX_NODES; i++)
        if (g_nodes[i].used && memcmp(g_nodes[i].uuid, uuid, 16) == 0)
            return true;
    return false;
}

static int add_node(uint16_t addr, const uint8_t *uuid,
                    const uint8_t *mac, const char *name)
{
    int idx = find_node_index(addr);
    if (idx >= 0) {
        if (uuid) memcpy(g_nodes[idx].uuid, uuid, 16);
        if (mac)  memcpy(g_nodes[idx].mac,  mac,  6);
        if (name && name[0])
            strncpy(g_nodes[idx].name, name, sizeof(g_nodes[idx].name)-1);
        return idx;
    }
    for (int i = 0; i < MAX_NODES; i++) {
        if (!g_nodes[i].used) {
            memset(&g_nodes[i], 0, sizeof(g_nodes[i]));
            g_nodes[i].used = true;
            g_nodes[i].addr = addr;
            if (uuid) memcpy(g_nodes[i].uuid, uuid, 16);
            if (mac)  memcpy(g_nodes[i].mac,  mac,  6);
            if (name && name[0])
                strncpy(g_nodes[i].name, name, sizeof(g_nodes[i].name)-1);
            else
                snprintf(g_nodes[i].name, sizeof(g_nodes[i].name), "Node_0x%04x", addr);
            return i;
        }
    }
    return -1;
}

static void log_node_table(void)
{
    printf("\n================ NODE TABLE ================\n");
    bool has_node = false;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    for (int i = 0; i < MAX_NODES; i++) {
        if (!g_nodes[i].used) continue;
        has_node = true;
        printf("Node %d\n", i+1);
        printf("  Address     : 0x%04X\n", g_nodes[i].addr);
        printf("  Name        : %s\n", g_nodes[i].name[0] ? g_nodes[i].name : "(unknown)");
        printf("  Type        : %s\n",
               g_nodes[i].is_relay   ? "RELAY"   :
               g_nodes[i].is_scanner ? "SCANNER" : "UNKNOWN");
        printf("  UUID        : "); print_uuid(g_nodes[i].uuid); printf("\n");
        printf("  MAC         : "); print_mac(g_nodes[i].mac);   printf("\n");
        printf("  Config done : %s\n", g_nodes[i].config_done ? "YES" : "NO");

        if (g_nodes[i].is_relay) {
            printf("  Status      : %s\n", g_nodes[i].online ? "ONLINE" : "OFFLINE");
            if (g_nodes[i].last_seen_ms > 0)
                printf("  LastSeen    : %" PRIu32 "s ago\n",
                       (now - g_nodes[i].last_seen_ms)/1000);
            else
                printf("  LastSeen    : never\n");
        } else if (g_nodes[i].is_scanner) {
            printf("  Status      : %s\n",
                   g_nodes[i].config_done ? "ACTIVE (AppKey bound)" : "PROVISIONED (configuring...)");
        }
        printf("------------------------------------------\n");
    }
    if (!has_node) {
        printf("  No provisioned nodes\n");
        printf("------------------------------------------\n");
    }
}

/* ========================= SCAN LIST HELPERS ========================= */
static void print_scan_list(void)
{
    printf("\n========== SCAN LIST ==========\n");
    if (g_scan_count == 0) {
        printf("  (empty)\n================================\n"); return;
    }
    for (int i = 0; i < g_scan_count; i++) {
        bool already = uuid_already_provisioned(g_scan_list[i].uuid);
        printf("  [%d] %-7s UUID: ", i, uuid_type_str(g_scan_list[i].uuid));
        print_uuid(g_scan_list[i].uuid);
        printf("\n        MAC : ");
        print_mac(g_scan_list[i].addr);
        printf("  %s\n", already ? "[ALREADY PROVISIONED]" : "[NEW]");
    }
    printf("================================\n");
}

static void do_scan(void)
{
    g_scan_count = 0;
    memset(g_scan_list, 0, sizeof(g_scan_list));
    g_scanning  = true;
    g_prov_mode = PROV_MODE_MANUAL;
    printf("\n[SCAN] Scanning %ds...\n", SCAN_DURATION_MS/1000);
    vTaskDelay(pdMS_TO_TICKS(SCAN_DURATION_MS));
    g_scanning = false;
    printf("[SCAN] Done.\n");
    print_scan_list();
}

static void provision_scan_list(void)
{
    int count = 0;
    for (int i = 0; i < g_scan_count; i++) {
        if (uuid_already_provisioned(g_scan_list[i].uuid)) {
            printf("[PROV] Skip [%d] already provisioned\n", i);
            continue;
        }
        esp_ble_mesh_unprov_dev_add_t dev = {0};
        memcpy(dev.uuid, g_scan_list[i].uuid, 16);
        memcpy(dev.addr, g_scan_list[i].addr,  6);
        dev.addr_type = g_scan_list[i].addr_type;
        dev.oob_info  = g_scan_list[i].oob_info;
        dev.bearer    = ESP_BLE_MESH_PROV_ADV;
        esp_err_t err = esp_ble_mesh_provisioner_add_unprov_dev(
            &dev, ADD_DEV_FLUSHABLE_DEV_FLAG | ADD_DEV_START_PROV_NOW_FLAG);
        if (err == ESP_OK) {
            printf("[PROV] Provisioning [%d] %s...\n", i, uuid_type_str(g_scan_list[i].uuid));
            count++;
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            printf("[PROV] Failed [%d]: %s\n", i, esp_err_to_name(err));
        }
    }
    if (count == 0) printf("[PROV] Nothing to provision.\n");
}

static void print_status(void)
{
    printf("\n=========== GATEWAY COMMANDS ===========\n");
    printf("1 -> LIST PROVISIONED NODES\n");
    printf("s -> SCAN BEACONS (%ds)\n", SCAN_DURATION_MS/1000);
    printf("p -> PROVISION SCAN LIST\n");
    printf("a -> AUTO PROVISION MODE\n");
    printf("4 -> SHOW STATUS\n");
    printf("0 -> CLEAR NVS (forget all nodes)\n");
    printf("Provision mode: %s\n",
           g_prov_mode == PROV_MODE_AUTO ? "AUTO" : "MANUAL");
    printf("=========================================\n");
}

/* ========================= WIFI ========================= */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi connected! IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_t any_id, got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &got_ip));
    wifi_config_t wifi_cfg = { .sta = { .threshold.authmode = WIFI_AUTH_WPA2_PSK } };
    strncpy((char *)wifi_cfg.sta.ssid,     WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strncpy((char *)wifi_cfg.sta.password, WIFI_PASS, sizeof(wifi_cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);
}

/* ========================= MQTT ========================= */
static void mqtt_event_handler(void *args, esp_event_base_t base,
                               int32_t id, void *data)
{
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        mqtt_connected = true;
        ESP_LOGI(TAG, "MQTT connected");
        break;
    case MQTT_EVENT_DISCONNECTED:
        mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    default: break;
    }
}

static void mqtt_init(void)
{
    esp_mqtt_client_config_t cfg = { .broker.address.uri = MQTT_BROKER };
    mqtt_client = esp_mqtt_client_init(&cfg);
    if (!mqtt_client) { ESP_LOGE(TAG, "MQTT init failed"); return; }
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

/* ========================= MQTT PUBLISH ========================= */
static void mqtt_publish_relay_status(uint16_t addr, bool online)
{
    if (!mqtt_connected || !mqtt_client) return;
    char topic[64], json[128];
    snprintf(topic, sizeof(topic), "ble/relay/0x%04x/status", addr);
    snprintf(json,  sizeof(json),
             "{\"addr\":\"0x%04x\",\"status\":\"%s\"}",
             addr, online ? "ONLINE" : "OFFLINE");
    esp_mqtt_client_publish(mqtt_client, topic, json, 0, 1, 1);
    ESP_LOGI(TAG, "MQTT [%s]: %s", topic, json);
}

static void mqtt_publish_gateway_online(void)
{
    if (!mqtt_connected || !mqtt_client) return;
    esp_mqtt_client_publish(mqtt_client, "ble/gateway/status",
                            "{\"status\":\"ONLINE\"}", 0, 1, 1);
}

static void mqtt_publish_tag_data(mesh_tag_report_t *r)
{
    if (!mqtt_connected || !mqtt_client || !r) return;
    char topic[64], json[256];
    /* Decode: distance_dm (×10) → float meters */
    float distance_m = r->distance_dm / 10.0f;
    snprintf(topic, sizeof(topic),
             "ble/scanner/0x%02x/tag/0x%04x",
             r->scanner_id, r->tag_id);
    snprintf(json, sizeof(json),
             "{\"scanner\":\"0x%02x\",\"tag\":\"0x%04x\","
             "\"type\":\"%s\",\"rssi\":%d,"
             "\"distance\":%.2f,\"loss\":%u}",
             r->scanner_id, r->tag_id,
             r->tag_type == 0x01 ? "PERSON" : "ASSET",
             r->rssi, distance_m, r->loss_pct);
    esp_mqtt_client_publish(mqtt_client, topic, json, 0, 1, 0);
    ESP_LOGI(TAG, "MQTT [%s]: %s", topic, json);
}

/* ========================= UART ========================= */
static void uart_cmd_init(void)
{
    const uart_config_t cfg = {
        .baud_rate  = GATEWAY_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(GATEWAY_UART_NUM,
                    GATEWAY_UART_RX_BUF_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(GATEWAY_UART_NUM, &cfg));
}

static void uart_cmd_task(void *arg)
{
    uint8_t ch;
    (void)arg;
    print_status();

    while (1) {
        int len = uart_read_bytes(GATEWAY_UART_NUM, &ch, 1, pdMS_TO_TICKS(200));
        if (len <= 0 || ch == '\r' || ch == '\n') continue;

        switch (ch) {
        case '1':
            log_node_table();
            break;
        case 's':
            printf("\n[UART] Starting MANUAL SCAN...\n");
            do_scan();
            break;
        case 'p':
            if (g_prov_mode != PROV_MODE_MANUAL)
                printf("\n[UART] Not in MANUAL mode. Press s first.\n");
            else
                provision_scan_list();
            break;
        case 'a':
            g_prov_mode  = PROV_MODE_AUTO;
            g_scan_count = 0;
            memset(g_scan_list, 0, sizeof(g_scan_list));
            printf("\n[UART] AUTO provision mode\n");
            break;
        case '4':
            print_status();
            break;
        case '0':
            printf("\n[UART] Clearing NVS...\n");
            nvs_clear_nodes();
            memset(g_nodes, 0, sizeof(g_nodes));
            printf("[UART] Done. Restart to re-provision.\n");
            break;
        default:
            printf("\n[UART] Unknown: %c\n", ch);
            print_status();
            break;
        }
    }
}

/* ========================= MESH CALLBACKS ========================= */

/* =========================================================
 * FIX: SCANNER CONFIG TASK
 *
 * Mục đích: gửi APP_KEY_ADD rồi mới MODEL_APP_BIND
 *
 * Tại sao phải làm 2 bước theo thứ tự này:
 *   1. APP_KEY_ADD  → scanner lưu AppKey vào local list
 *                   → mesh_config_server_cb trên scanner fires
 *                   → g_app_idx được set → scanner có thể publish
 *   2. MODEL_APP_BIND → bind AppKey đó vào Vendor Model
 *                     → model mới dùng được AppKey để send/recv
 *
 * Tại sao dùng task riêng (không inline trong callback):
 *   Mesh callback chạy trong mesh internal task
 *   Gọi vTaskDelay trong callback sẽ block toàn bộ mesh stack
 *   → Dùng task riêng để delay an toàn
 * ========================================================= */
static void scanner_config_task(void *arg)
{
    uint16_t addr = (uint16_t)(uint32_t)arg;
    ESP_LOGI(TAG, "[SCN_CFG] Configuring scanner 0x%04x...", addr);

    /* Đợi mesh ổn định sau provision */
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* -------- STEP 1: APP_KEY_ADD --------
     * BẮT BUỘC phải làm trước MODEL_APP_BIND
     * Khi scanner nhận: mesh_config_server_cb fires với APP_KEY_ADD
     * → g_app_idx = 0x0000 → scanner sẽ publish được */
    {
        esp_ble_mesh_client_common_param_t  c = {0};
        esp_ble_mesh_cfg_client_set_state_t s = {0};
        c.opcode              = ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD;
        c.model               = &root_models[1];  /* Config Client */
        c.ctx.net_idx         = net_key_idx;
        c.ctx.app_idx         = 0xFFFF;           /* device key cho config msg */
        c.ctx.addr            = addr;
        c.ctx.send_ttl        = 7;
        c.msg_timeout         = 8000;
        s.app_key_add.net_idx = net_key_idx;
        s.app_key_add.app_idx = app_key_idx;
        memcpy(s.app_key_add.app_key, local_app_key, 16);

        esp_err_t e = esp_ble_mesh_config_client_set_state(&c, &s);
        if (e == ESP_OK)
            ESP_LOGI(TAG, "[SCN_CFG] Step 1: APP_KEY_ADD sent to 0x%04x", addr);
        else
            ESP_LOGE(TAG, "[SCN_CFG] Step 1: APP_KEY_ADD FAILED: %s", esp_err_to_name(e));
    }

    /* Đợi scanner xử lý APP_KEY_ADD */
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* -------- STEP 2: MODEL_APP_BIND --------
     * Chỉ làm được SAU KHI APP_KEY_ADD thành công
     * Bind AppKey vào Vendor Model của scanner
     * Sau bước này scanner có thể gửi message với AppKey */
    {
        esp_ble_mesh_client_common_param_t  c = {0};
        esp_ble_mesh_cfg_client_set_state_t s = {0};
        c.opcode                    = ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND;
        c.model                     = &root_models[1];
        c.ctx.net_idx               = net_key_idx;
        c.ctx.app_idx               = 0xFFFF;
        c.ctx.addr                  = addr;
        c.ctx.send_ttl              = 7;
        c.msg_timeout               = 5000;
        s.model_app_bind.element_addr  = addr;
        s.model_app_bind.model_app_idx = app_key_idx;
        s.model_app_bind.model_id      = ESP_BLE_MESH_VND_MODEL_ID;
        s.model_app_bind.company_id    = CID_ESP;

        esp_err_t e = esp_ble_mesh_config_client_set_state(&c, &s);
        if (e == ESP_OK)
            ESP_LOGI(TAG, "[SCN_CFG] Step 2: MODEL_APP_BIND sent to 0x%04x", addr);
        else
            ESP_LOGE(TAG, "[SCN_CFG] Step 2: MODEL_APP_BIND FAILED: %s", esp_err_to_name(e));
    }

    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Đánh dấu scanner đã được config xong */
    int idx = find_node_index(addr);
    if (idx >= 0) {
        g_nodes[idx].config_done = true;
        nvs_save_nodes();
    }
    ESP_LOGI(TAG, "[SCN_CFG] Scanner 0x%04x fully configured!", addr);
    log_node_table();

    vTaskDelete(NULL);
}

static void mesh_prov_cb(esp_ble_mesh_prov_cb_event_t event,
                         esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_PROV_REGISTER_COMP_EVT:
        ESP_LOGI(TAG, "Provisioner registered");
        break;

    case ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(TAG, "Provisioner scan enabled");
        /* AppKey bind đã được xử lý trong ble_mesh_init_gateway()
         * bằng cách gán trực tiếp vnd_models[0].keys[0] = app_key_idx
         * KHÔNG gọi bind ở đây vì AppKey chưa được add tại thời điểm này */
        break;

    case ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT: {
        const uint8_t *uuid      = param->provisioner_recv_unprov_adv_pkt.dev_uuid;
        const uint8_t *mac       = param->provisioner_recv_unprov_adv_pkt.addr;
        uint8_t        addr_type = param->provisioner_recv_unprov_adv_pkt.addr_type;
        uint16_t       oob_info  = param->provisioner_recv_unprov_adv_pkt.oob_info;

        mac_cache_store(uuid, mac);

        if (g_prov_mode == PROV_MODE_MANUAL) {
            if (!g_scanning) break;
            for (int i = 0; i < g_scan_count; i++)
                if (memcmp(g_scan_list[i].uuid, uuid, 16) == 0) goto scan_dup;
            if (g_scan_count < MAX_SCAN_LIST) {
                memcpy(g_scan_list[g_scan_count].uuid, uuid, 16);
                memcpy(g_scan_list[g_scan_count].addr, mac,  6);
                g_scan_list[g_scan_count].addr_type = addr_type;
                g_scan_list[g_scan_count].oob_info  = oob_info;
                g_scan_list[g_scan_count].used      = true;
                g_scan_count++;
                printf("[SCAN] [%d] %-7s MAC:", g_scan_count-1, uuid_type_str(uuid));
                for (int b = 0; b < 6; b++) printf("%02X%s", mac[b], b<5?":":"");
                printf("\n");
            }
            scan_dup: break;
        }

        if (uuid_already_provisioned(uuid)) break;

        ESP_LOGI(TAG, "Found unprovisioned [%s]", uuid_type_str(uuid));
        esp_ble_mesh_unprov_dev_add_t dev = {0};
        memcpy(dev.uuid, uuid, 16);
        memcpy(dev.addr, mac,  6);
        dev.addr_type = addr_type;
        dev.oob_info  = oob_info;
        dev.bearer    = ESP_BLE_MESH_PROV_ADV;
        esp_ble_mesh_provisioner_add_unprov_dev(
            &dev, ADD_DEV_FLUSHABLE_DEV_FLAG | ADD_DEV_START_PROV_NOW_FLAG);
        break;
    }

    case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT: {
        uint16_t       addr = param->provisioner_prov_complete.unicast_addr;
        const uint8_t *uuid = param->provisioner_prov_complete.device_uuid;
        ESP_LOGI(TAG, "Provision complete addr=0x%04x type=%s", addr, uuid_type_str(uuid));

        uint8_t mac[6] = {0};
        mac_cache_get(uuid, mac);

        int idx = add_node(addr, uuid, mac, NULL);
        if (idx < 0) { ESP_LOGW(TAG, "Node table full"); break; }

        /* RELAY: không cần AppKey bind */
        if (uuid_is_relay(uuid)) {
            g_nodes[idx].is_relay     = true;
            g_nodes[idx].is_scanner   = false;
            g_nodes[idx].online       = false;
            g_nodes[idx].last_seen_ms = 0;
            g_nodes[idx].config_done  = true;   /* relay không cần config thêm */
            snprintf(g_nodes[idx].name, sizeof(g_nodes[idx].name),
                     "Relay_0x%04x", addr);
            ESP_LOGI(TAG, "Node 0x%04x = RELAY", addr);
            nvs_save_nodes();
            log_node_table();
            break;
        }

        /* SCANNER: FIX — dùng task riêng để APP_KEY_ADD → MODEL_APP_BIND
         * KHÔNG dùng vTaskDelay trong callback (sẽ block mesh stack)
         * KHÔNG gọi trực tiếp MODEL_APP_BIND (thiếu bước APP_KEY_ADD) */
        if (uuid_is_scanner(uuid)) {
            g_nodes[idx].is_scanner   = true;
            g_nodes[idx].is_relay     = false;
            g_nodes[idx].config_done  = false;  /* chưa config xong */
            snprintf(g_nodes[idx].name, sizeof(g_nodes[idx].name),
                     "Scanner_0x%04x", addr);
            ESP_LOGI(TAG, "Node 0x%04x = SCANNER, launching config task...", addr);
            nvs_save_nodes();
            log_node_table();

            /* Kick off config task: APP_KEY_ADD → MODEL_APP_BIND */
            xTaskCreate(scanner_config_task, "scan_cfg", 3072,
                        (void *)(uint32_t)addr, 5, NULL);
            break;
        }

        ESP_LOGW(TAG, "Unknown node type");
        nvs_save_nodes();
        log_node_table();
        break;
    }

    default: break;
    }
}

/* FIX: thêm xử lý SET_STATE_EVT và TIMEOUT để debug config */
static void mesh_config_client_cb(esp_ble_mesh_cfg_client_cb_event_t event,
                                  esp_ble_mesh_cfg_client_cb_param_t *param)
{
    if (!param || !param->params) return;
    uint16_t addr = param->params->ctx.addr;
    int      idx  = find_node_index(addr);

    /* ACK từ các bước config scanner */
    if (event == ESP_BLE_MESH_CFG_CLIENT_SET_STATE_EVT) {
        uint32_t opcode = param->params->opcode;

        if (opcode == ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD) {
            ESP_LOGI(TAG, "[CFG] APP_KEY_ADD ACK from 0x%04x — AppKey delivered!", addr);
        }

        if (opcode == ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND) {
            ESP_LOGI(TAG, "[CFG] MODEL_APP_BIND ACK from 0x%04x", addr);
            if (idx >= 0 && g_nodes[idx].is_scanner) {
                ESP_LOGI(TAG, "=== Scanner 0x%04x READY — will publish tag data ===", addr);
            }
        }

        if (opcode == ESP_BLE_MESH_MODEL_OP_RELAY_SET) {
            ESP_LOGI(TAG, "[CFG] RELAY_SET ACK from 0x%04x", addr);
        }
    }

    /* Timeout — báo để debug */
    if (event == ESP_BLE_MESH_CFG_CLIENT_TIMEOUT_EVT) {
        uint32_t opcode = param->params->opcode;
        ESP_LOGW(TAG, "[CFG] TIMEOUT opcode=0x%04" PRIx32 " addr=0x%04x — retransmit or check mesh coverage",
                 opcode, addr);
    }

    /* Reply từ ping relay (GET_STATE) */
    if (event == ESP_BLE_MESH_CFG_CLIENT_GET_STATE_EVT) {
        if (idx >= 0 && g_nodes[idx].is_relay) {
            g_nodes[idx].last_seen_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (!g_nodes[idx].online) {
                g_nodes[idx].online = true;
                ESP_LOGI(TAG, "Relay 0x%04x ONLINE", addr);
                mqtt_publish_relay_status(addr, true);
            }
        }
    }
}

static void mesh_config_server_cb(esp_ble_mesh_cfg_server_cb_event_t event,
                                  esp_ble_mesh_cfg_server_cb_param_t *param)
{
    (void)param;
    ESP_LOGI(TAG, "Config server event: %d", event);
}

/* ========================= VENDOR MODEL CALLBACK =========================
 * Nhận data từ Scanner qua Vendor Model
 * FIX: log ALL events để debug, không chỉ MODEL_OPERATION_EVT
 * ========================================================================= */
static void mesh_vnd_client_cb(esp_ble_mesh_model_cb_event_t event,
                               esp_ble_mesh_model_cb_param_t *param)
{
    /* Debug: log tất cả events để biết cái nào fire */
    ESP_LOGI(TAG, "[VND] Callback event=%d", event);

    if (event != ESP_BLE_MESH_MODEL_OPERATION_EVT) return;
    if (!param) return;

    uint32_t opcode = param->model_operation.opcode;
    uint16_t src    = param->model_operation.ctx->addr;
    uint8_t *data   = param->model_operation.msg;
    uint16_t len    = param->model_operation.length;

    ESP_LOGI(TAG, "[VND] Received opcode=0x%06" PRIx32
             " src=0x%04x len=%d", opcode, src, len);

    if (opcode != OP_VND_TAG_STATUS) return;
    if (len < sizeof(mesh_tag_report_t)) {
        ESP_LOGW(TAG, "[VND] Payload too short: %d < %d",
                 len, (int)sizeof(mesh_tag_report_t));
        return;
    }

    mesh_tag_report_t report;
    memcpy(&report, data, sizeof(report));

    /* Decode: distance_dm ÷ 10 = meters */
    float distance_m = report.distance_dm / 10.0f;

    ESP_LOGI(TAG, "[VND] Tag: scanner=0x%02x tag=0x%04x type=%s "
             "rssi=%ddBm dist=%.2fm loss=%u%%",
             report.scanner_id, report.tag_id,
             report.tag_type == 0x01 ? "PERSON" : "ASSET",
             report.rssi, distance_m, report.loss_pct);

    mqtt_publish_tag_data(&report);
}

/* ========================= TASKS ========================= */
static void relay_ping_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(15000));

    while (1) {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        for (int i = 0; i < MAX_NODES; i++) {
            if (!g_nodes[i].used || !g_nodes[i].is_relay) continue;

            esp_ble_mesh_client_common_param_t  common = {0};
            esp_ble_mesh_cfg_client_get_state_t get    = {0};
            common.opcode       = ESP_BLE_MESH_MODEL_OP_DEFAULT_TTL_GET;
            common.model        = &root_models[1];
            common.ctx.net_idx  = net_key_idx;
            common.ctx.app_idx  = 0xFFFF;
            common.ctx.addr     = g_nodes[i].addr;
            common.ctx.send_ttl = 7;
            common.msg_timeout  = 5000;
            esp_err_t err = esp_ble_mesh_config_client_get_state(&common, &get);
            if (err != ESP_OK)
                ESP_LOGW(TAG, "Ping relay 0x%04X failed", g_nodes[i].addr);

            if (g_nodes[i].last_seen_ms > 0 &&
                (now - g_nodes[i].last_seen_ms) > RELAY_OFFLINE_TIMEOUT_MS) {
                if (g_nodes[i].online) {
                    g_nodes[i].online = false;
                    ESP_LOGW(TAG, "Relay 0x%04X OFFLINE", g_nodes[i].addr);
                    mqtt_publish_relay_status(g_nodes[i].addr, false);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        vTaskDelay(pdMS_TO_TICKS(RELAY_PING_INTERVAL_MS));
    }
}

static void watchdog_feed_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    while (1) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

/* ========================= MESH INIT ========================= */
static esp_err_t ble_mesh_init_gateway(void)
{
    esp_err_t err;

    esp_ble_mesh_register_prov_callback(mesh_prov_cb);
    esp_ble_mesh_register_config_client_callback(mesh_config_client_cb);
    esp_ble_mesh_register_config_server_callback(mesh_config_server_cb);
    esp_ble_mesh_register_custom_model_callback(mesh_vnd_client_cb);

    err = esp_ble_mesh_init(&provision, &composition);
    if (err != ESP_OK) return err;

    err = esp_ble_mesh_provisioner_set_dev_uuid_match(NULL, 0, 0, false);
    if (err != ESP_OK) return err;

    err = esp_ble_mesh_provisioner_prov_enable(
        ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
    if (err != ESP_OK) return err;

    /* Add NetKey */
    const uint8_t *exist_net =
        esp_ble_mesh_provisioner_get_local_net_key(net_key_idx);
    if (!exist_net) {
        err = esp_ble_mesh_provisioner_add_local_net_key(local_net_key, net_key_idx);
        if (err != ESP_OK) return err;
        ESP_LOGI(TAG, "NetKey added");
    }

    /* Add AppKey */
    const uint8_t *exist_app =
        esp_ble_mesh_provisioner_get_local_app_key(net_key_idx, app_key_idx);
    if (!exist_app) {
        err = esp_ble_mesh_provisioner_add_local_app_key(
            local_app_key, net_key_idx, app_key_idx);
        if (err != ESP_OK) return err;
        ESP_LOGI(TAG, "AppKey added");
    }

    /* FIX: Gán AppKey vào vendor model trực tiếp qua keys[] array
     *
     * Vấn đề: esp_ble_mesh_provisioner_bind_app_key_to_local_model() fail trong
     * ESP-IDF v6.0 với lỗi "No model found, model id 0x0000, cid 0x0000"
     *   - Gọi trong PROV_ENABLE_COMP_EVT: quá sớm (AppKey chưa được add)
     *   - Gọi ở đây: API nội bộ fail (có thể do thay đổi trong v6.0)
     *
     * Workaround: gán trực tiếp vào model->keys[0]
     * - vnd_models[0].keys[i] khởi tạo = 0xFFFF (ESP_BLE_MESH_KEY_UNUSED)
     * - Khi stack nhận message với AppKey 0x0000, nó check keys[] của model
     * - Nếu keys[0] = 0x0000 → match → deliver lên application callback
     *
     * AppKey material (16 bytes thực sự) đã được add qua add_local_app_key
     * Chỉ cần model biết nó được bind với index 0x0000 là đủ */
    vnd_models[0].keys[0] = app_key_idx;   /* 0x0000 */
    ESP_LOGI(TAG, "Gateway vendor model AppKey bound directly: keys[0]=0x%04x", app_key_idx);

    ESP_LOGI(TAG, "BLE Mesh Gateway init OK");
    return ESP_OK;
}

/* ========================= MAIN ========================= */
void app_main(void)
{
    esp_err_t err;
    ESP_LOGI(TAG, "=== BLE Mesh Gateway (Tracking) Starting ===");

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    nvs_load_nodes();

    wifi_init();
    mqtt_init();
    vTaskDelay(pdMS_TO_TICKS(2000));

    err = bluetooth_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BT init failed: %s", esp_err_to_name(err)); return;
    }

    err = ble_mesh_init_gateway();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Mesh init failed: %s", esp_err_to_name(err)); return;
    }

    uart_cmd_init();

    printf("\n================ GATEWAY TRACKING ================\n");
    print_hex_key("NetKey: ", local_net_key, 16);
    print_hex_key("AppKey: ", local_app_key, 16);
    printf("MQTT  : %s\n", MQTT_BROKER);
    printf("===================================================\n");

    print_status();
    log_node_table();
    mqtt_publish_gateway_online();

    /* FIX WDT: Cấu hình WDT TRƯỚC khi tạo watchdog_feed_task
     * watchdog_feed_task tự add chính nó vào WDT monitoring
     *
     * KHÔNG gọi esp_task_wdt_add(NULL) ở đây!
     * Lý do: NULL = main_task, nhưng app_main sẽ return ngay
     * → main_task bị xóa → WDT vẫn monitor task đã chết
     * → sau 60s không có reset → WDT crash với tên task bị garbled (▒C?) */
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = GATEWAY_WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    esp_task_wdt_reconfigure(&wdt_cfg);
    /* esp_task_wdt_add(NULL) ← ĐÃ XÓA: KHÔNG add main_task vào WDT */

    xTaskCreate(uart_cmd_task,      "uart_cmd",   4096, NULL, 4, NULL);
    xTaskCreate(relay_ping_task,    "relay_ping", 4096, NULL, 3, NULL);
    xTaskCreate(watchdog_feed_task, "wdt_feed",   2048, NULL, 2, NULL);

    ESP_LOGI(TAG, "=== Gateway READY ===");
}