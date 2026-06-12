/* ============================================================================
 * BMT (BLE Mesh Tracking) — GATEWAY firmware
 * ----------------------------------------------------------------------------
 * Role  : Provisioner + BLE Mesh ↔ ThingsBoard MQTT bridge
 * Board : ESP32 NodeMCU-32S
 * IDF   : v6.0
 *
 * Naming convention:
 *   - Macros          : BMT_<CATEGORY>_<NAME>
 *   - Functions       : bmt_<module>_<action>()
 *   - Structs         : bmt_<scope>_<name>_t
 *   - Globals         : g_bmt_<name>
 *
 * ThingsBoard integration (Gateway API):
 *   - Topic publish telemetry    : v1/gateway/telemetry
 *   - Topic publish attributes   : v1/gateway/attributes
 *   - Topic connect sub-device   : v1/gateway/connect
 *   - Topic disconnect           : v1/gateway/disconnect
 *   - Gateway self telemetry     : v1/devices/me/telemetry
 *   - Gateway self attributes    : v1/devices/me/attributes
 *
 * Device naming on ThingsBoard:
 *   - bmt_gateway                 (role=gateway)
 *   - bmt_node_0xXXXX             (role=relay  | role=scan)
 *   - bmt_tag_0xYYYY              (role=tag)
 * ============================================================================ */

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

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define TAG                             "BMT_GW"

/* ============================================================================
 * USER CONFIG
 * ============================================================================ */
#define BMT_WIFI_SSID                   "TP-Link_385B"
#define BMT_WIFI_PASS                   "91324566"

/* ThingsBoard server (IP của máy PC chạy Docker, port 1883 MQTT) */
#define BMT_TB_HOST                     "mqtt://192.168.1.113:1883"
#define BMT_TB_GATEWAY_TOKEN            "Sxj62zRhBtUj3tZIRdhd"

/* Device naming prefix (must match ThingsBoard expectations) */
#define BMT_DEV_NAME_GATEWAY            "bmt_gateway"
#define BMT_DEV_NAME_NODE_FMT           "bmt_node_0x%04x"
#define BMT_DEV_NAME_TAG_FMT            "bmt_tag_0x%04x"

/* Role attribute values */
#define BMT_ROLE_GATEWAY                "gateway"
#define BMT_ROLE_RELAY                  "relay"
#define BMT_ROLE_SCAN                   "scan"
#define BMT_ROLE_TAG                    "tag"

/* ============================================================================
 * BLE MESH VENDOR MODEL
 *   CID  : 0x02E5 (Espressif)
 *   Gateway = Vendor Client (nhận data từ Scan Node)
 * ============================================================================ */
#define BMT_CID_ESP                     0x02E5
#define BMT_VND_MODEL_ID                0x0000
#define BMT_OP_VND_TAG_STATUS           ESP_BLE_MESH_MODEL_OP_3(0x00, BMT_CID_ESP)

#pragma pack(1)
/*
 * Payload BLE Mesh từ Scan Node → Gateway
 *
 * FIX: giảm payload từ 13 xuống 8 bytes
 *   BLE Mesh unsegmented max = 11 bytes (opcode 3B + data 8B)
 *   13 bytes → segmented → cần ACK → "Ran out of retransmit"
 *   8 bytes  → unsegmented → fire-and-forget, không cần ACK
 *
 * distance_dm : int16_t đơn vị decimeter, chia 10 để ra mét
 *               max = 3276.7m, resolution = 0.1m
 * loss_pct    : uint8_t 0-100%
 */
typedef struct {
    uint8_t  scanner_id;    /* 1B : Scanner nào gửi report          */
    uint8_t  tag_type;      /* 1B : PERSON=0x01 / ASSET=0x02        */
    uint16_t tag_id;        /* 2B : ID của tag                      */
    int8_t   rssi;          /* 1B : RSSI filtered (dBm)             */
    int16_t  distance_dm;   /* 2B : distance × 10 (decimeter)       */
    uint8_t  loss_pct;      /* 1B : loss rate % (0-100)             */
} bmt_tag_report_t;         /* total = 8 bytes → UNSEGMENTED ✓     */
#pragma pack()

/* ============================================================================
 * TIMING / RESOURCES
 * ============================================================================ */
#define BMT_MAX_NODES                   10
#define BMT_MAX_SCAN_LIST               10
#define BMT_MAX_MAC_CACHE               16
#define BMT_MAX_SEEN_TAGS               16

#define BMT_RELAY_PING_INTERVAL_MS      20000
#define BMT_RELAY_OFFLINE_TIMEOUT_MS    60000
#define BMT_SCAN_DURATION_MS            10000
#define BMT_WDT_TIMEOUT_S               60

/* NVS */
#define BMT_NVS_NAMESPACE               "bmt_gw"
#define BMT_NVS_KEY_NODES               "node_table"

/* UART */
#define BMT_UART_NUM                    UART_NUM_0
#define BMT_UART_BAUD                   115200
#define BMT_UART_RX_BUF_SIZE            2048

/* ============================================================================
 * TYPES
 * ============================================================================ */
typedef enum {
    BMT_PROV_MODE_AUTO   = 0,
    BMT_PROV_MODE_MANUAL = 1,
} bmt_gateway_prov_mode_t;

typedef struct {
    bool    used;
    uint8_t uuid[16];
    uint8_t mac[6];
} bmt_gateway_mac_cache_t;

typedef struct {
    bool     used;
    bool     is_relay;
    bool     is_scan;
    bool     config_done;
    bool     online;
    uint32_t last_seen_ms;
    uint16_t addr;
    uint8_t  uuid[16];
    uint8_t  mac[6];
    char     name[32];
} bmt_gateway_node_t;

typedef struct {
    bool     used;
    uint8_t  uuid[16];
    uint8_t  addr[6];
    uint8_t  addr_type;
    uint16_t oob_info;
} bmt_gateway_scan_entry_t;

/* ============================================================================
 * GLOBALS
 * ============================================================================ */
static bmt_gateway_mac_cache_t   g_bmt_mac_cache[BMT_MAX_MAC_CACHE];
static bmt_gateway_node_t        g_bmt_nodes[BMT_MAX_NODES];
static bmt_gateway_scan_entry_t  g_bmt_scan_list[BMT_MAX_SCAN_LIST];
static int                       g_bmt_scan_count = 0;
static bool                      g_bmt_scanning   = false;
static bmt_gateway_prov_mode_t   g_bmt_prov_mode  = BMT_PROV_MODE_MANUAL;

/* Mesh keys */
static uint16_t g_bmt_net_key_idx = 0x0000;
static uint16_t g_bmt_app_key_idx = 0x0000;

static const uint8_t g_bmt_net_key[16] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef
};
static const uint8_t g_bmt_app_key[16] = {
    0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
    0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10
};
static uint8_t g_bmt_gw_uuid[ESP_BLE_MESH_OCTET16_LEN] = {
    0x47, 0x41, 0x54, 0x45, 0x57, 0x41, 0x59, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01
};

/* WiFi / MQTT */
static EventGroupHandle_t       g_bmt_wifi_evgrp;
static const int                BMT_WIFI_CONNECTED_BIT = BIT0;
static esp_mqtt_client_handle_t g_bmt_mqtt_client    = NULL;
static bool                     g_bmt_mqtt_connected = false;

/* Mesh models */
static esp_ble_mesh_cfg_srv_t bmt_cfg_server = {
    .net_transmit     = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay            = ESP_BLE_MESH_RELAY_ENABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon           = ESP_BLE_MESH_BEACON_ENABLED,
    .gatt_proxy       = ESP_BLE_MESH_GATT_PROXY_ENABLED,
    .friend_state     = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .default_ttl      = 7,
};

static esp_ble_mesh_client_t bmt_cfg_client;
static esp_ble_mesh_client_t bmt_vnd_client;

static esp_ble_mesh_model_op_t bmt_vnd_ops[] = {
    ESP_BLE_MESH_MODEL_OP(BMT_OP_VND_TAG_STATUS, sizeof(bmt_tag_report_t)),
    ESP_BLE_MESH_MODEL_OP_END,
};

static esp_ble_mesh_model_t bmt_vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(BMT_CID_ESP, BMT_VND_MODEL_ID,
                              bmt_vnd_ops, NULL, &bmt_vnd_client),
};

static esp_ble_mesh_model_t bmt_root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&bmt_cfg_server),
    ESP_BLE_MESH_MODEL_CFG_CLI(&bmt_cfg_client),
};

static esp_ble_mesh_elem_t bmt_elements[] = {
    ESP_BLE_MESH_ELEMENT(0, bmt_root_models, bmt_vnd_models),
};

static esp_ble_mesh_comp_t bmt_composition = {
    .cid           = BMT_CID_ESP,
    .element_count = ARRAY_SIZE(bmt_elements),
    .elements      = bmt_elements,
};

static esp_ble_mesh_prov_t bmt_provision = {
    .uuid               = g_bmt_gw_uuid,
    .prov_unicast_addr  = 0x0001,
    .prov_start_address = 0x0002,
};

/* ============================================================================
 * MAC CACHE
 * ============================================================================ */
static void bmt_mac_cache_store(const uint8_t *uuid, const uint8_t *mac)
{
    for (int i = 0; i < BMT_MAX_MAC_CACHE; i++) {
        if (g_bmt_mac_cache[i].used &&
            memcmp(g_bmt_mac_cache[i].uuid, uuid, 16) == 0) {
            memcpy(g_bmt_mac_cache[i].mac, mac, 6);
            return;
        }
    }
    for (int i = 0; i < BMT_MAX_MAC_CACHE; i++) {
        if (!g_bmt_mac_cache[i].used) {
            g_bmt_mac_cache[i].used = true;
            memcpy(g_bmt_mac_cache[i].uuid, uuid, 16);
            memcpy(g_bmt_mac_cache[i].mac,  mac,  6);
            return;
        }
    }
}

static bool bmt_mac_cache_get(const uint8_t *uuid, uint8_t *mac_out)
{
    for (int i = 0; i < BMT_MAX_MAC_CACHE; i++) {
        if (g_bmt_mac_cache[i].used &&
            memcmp(g_bmt_mac_cache[i].uuid, uuid, 16) == 0) {
            memcpy(mac_out, g_bmt_mac_cache[i].mac, 6);
            return true;
        }
    }
    return false;
}

/* ============================================================================
 * NVS
 * ============================================================================ */
static void bmt_nvs_save_nodes(void)
{
    nvs_handle_t h;
    if (nvs_open(BMT_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, BMT_NVS_KEY_NODES, g_bmt_nodes, sizeof(g_bmt_nodes)) == ESP_OK)
        nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Node table saved to NVS");
}

static void bmt_nvs_load_nodes(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(BMT_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved node table, starting fresh");
        return;
    }
    if (err != ESP_OK) return;
    size_t size = sizeof(g_bmt_nodes);
    err = nvs_get_blob(h, BMT_NVS_KEY_NODES, g_bmt_nodes, &size);
    nvs_close(h);
    if (err == ESP_OK) {
        int count = 0;
        for (int i = 0; i < BMT_MAX_NODES; i++) {
            if (!g_bmt_nodes[i].used) continue;
            count++;
            if (g_bmt_nodes[i].is_relay) {
                g_bmt_nodes[i].online       = false;
                g_bmt_nodes[i].last_seen_ms = 0;
            }
        }
        ESP_LOGI(TAG, "Node table loaded (%d nodes)", count);
    }
}

static void bmt_nvs_clear_nodes(void)
{
    nvs_handle_t h;
    if (nvs_open(BMT_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, BMT_NVS_KEY_NODES);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "NVS cleared");
}

/* ============================================================================
 * HELPERS
 * ============================================================================ */
static void bmt_print_hex_key(const char *label, const uint8_t *key, int len)
{
    printf("%s", label);
    for (int i = 0; i < len; i++) {
        printf("%02X", key[i]);
        if (i != len-1) printf(":");
    }
    printf("\n");
}

static void bmt_print_uuid(const uint8_t *uuid)
{
    if (!uuid) return;
    for (int i = 0; i < 16; i++) {
        printf("%02X", uuid[i]);
        if (i != 15) printf(":");
    }
}

static void bmt_print_mac(const uint8_t *mac)
{
    if (!mac) return;
    for (int i = 0; i < 6; i++) {
        printf("%02X", mac[i]);
        if (i != 5) printf(":");
    }
}

static bool bmt_uuid_is_relay(const uint8_t *uuid)
{
    if (!uuid) return false;
    return (uuid[0] == 0x52 && uuid[1] == 0x45 &&
            uuid[2] == 0x4C && uuid[3] == 0x41 && uuid[4] == 0x59);
}

static bool bmt_uuid_is_scan(const uint8_t *uuid)
{
    if (!uuid) return false;
    return (uuid[0] == 0x53 && uuid[1] == 0x43 &&
            uuid[2] == 0x41 && uuid[3] == 0x4E);
}

static const char *bmt_uuid_type_str(const uint8_t *uuid)
{
    if (bmt_uuid_is_relay(uuid)) return "RELAY";
    if (bmt_uuid_is_scan(uuid))  return "SCAN";
    return "UNKNOWN";
}

static int bmt_find_node_index(uint16_t addr)
{
    for (int i = 0; i < BMT_MAX_NODES; i++)
        if (g_bmt_nodes[i].used && g_bmt_nodes[i].addr == addr) return i;
    return -1;
}

static bool bmt_uuid_already_provisioned(const uint8_t *uuid)
{
    for (int i = 0; i < BMT_MAX_NODES; i++)
        if (g_bmt_nodes[i].used && memcmp(g_bmt_nodes[i].uuid, uuid, 16) == 0)
            return true;
    return false;
}

static int bmt_add_node(uint16_t addr, const uint8_t *uuid,
                        const uint8_t *mac, const char *name)
{
    int idx = bmt_find_node_index(addr);
    if (idx >= 0) {
        if (uuid) memcpy(g_bmt_nodes[idx].uuid, uuid, 16);
        if (mac)  memcpy(g_bmt_nodes[idx].mac,  mac,  6);
        if (name && name[0])
            strncpy(g_bmt_nodes[idx].name, name, sizeof(g_bmt_nodes[idx].name)-1);
        return idx;
    }
    for (int i = 0; i < BMT_MAX_NODES; i++) {
        if (!g_bmt_nodes[i].used) {
            memset(&g_bmt_nodes[i], 0, sizeof(g_bmt_nodes[i]));
            g_bmt_nodes[i].used = true;
            g_bmt_nodes[i].addr = addr;
            if (uuid) memcpy(g_bmt_nodes[i].uuid, uuid, 16);
            if (mac)  memcpy(g_bmt_nodes[i].mac,  mac,  6);
            if (name && name[0])
                strncpy(g_bmt_nodes[i].name, name, sizeof(g_bmt_nodes[i].name)-1);
            else
                snprintf(g_bmt_nodes[i].name, sizeof(g_bmt_nodes[i].name),
                         "Node_0x%04x", addr);
            return i;
        }
    }
    return -1;
}

static void bmt_log_node_table(void)
{
    printf("\n================ NODE TABLE ================\n");
    bool has_node = false;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    for (int i = 0; i < BMT_MAX_NODES; i++) {
        if (!g_bmt_nodes[i].used) continue;
        has_node = true;
        printf("Node %d\n", i+1);
        printf("  Address     : 0x%04X\n", g_bmt_nodes[i].addr);
        printf("  Name        : %s\n",
               g_bmt_nodes[i].name[0] ? g_bmt_nodes[i].name : "(unknown)");
        printf("  Type        : %s\n",
               g_bmt_nodes[i].is_relay ? "RELAY"   :
               g_bmt_nodes[i].is_scan  ? "SCAN"    : "UNKNOWN");
        printf("  UUID        : "); bmt_print_uuid(g_bmt_nodes[i].uuid); printf("\n");
        printf("  MAC         : "); bmt_print_mac(g_bmt_nodes[i].mac);   printf("\n");
        printf("  Config done : %s\n", g_bmt_nodes[i].config_done ? "YES" : "NO");

        if (g_bmt_nodes[i].is_relay) {
            printf("  Status      : %s\n", g_bmt_nodes[i].online ? "ONLINE" : "OFFLINE");
            if (g_bmt_nodes[i].last_seen_ms > 0)
                printf("  LastSeen    : %" PRIu32 "s ago\n",
                       (now - g_bmt_nodes[i].last_seen_ms)/1000);
            else
                printf("  LastSeen    : never\n");
        } else if (g_bmt_nodes[i].is_scan) {
            printf("  Status      : %s\n",
                   g_bmt_nodes[i].config_done
                       ? "ACTIVE (AppKey bound)"
                       : "PROVISIONED (configuring...)");
        }
        printf("------------------------------------------\n");
    }
    if (!has_node) {
        printf("  No provisioned nodes\n");
        printf("------------------------------------------\n");
    }
}

/* ============================================================================
 * SCAN LIST
 * ============================================================================ */
static void bmt_print_scan_list(void)
{
    printf("\n========== SCAN LIST ==========\n");
    if (g_bmt_scan_count == 0) {
        printf("  (empty)\n================================\n"); return;
    }
    for (int i = 0; i < g_bmt_scan_count; i++) {
        bool already = bmt_uuid_already_provisioned(g_bmt_scan_list[i].uuid);
        printf("  [%d] %-7s UUID: ", i, bmt_uuid_type_str(g_bmt_scan_list[i].uuid));
        bmt_print_uuid(g_bmt_scan_list[i].uuid);
        printf("\n        MAC : ");
        bmt_print_mac(g_bmt_scan_list[i].addr);
        printf("  %s\n", already ? "[ALREADY PROVISIONED]" : "[NEW]");
    }
    printf("================================\n");
}

static void bmt_do_scan(void)
{
    g_bmt_scan_count = 0;
    memset(g_bmt_scan_list, 0, sizeof(g_bmt_scan_list));
    g_bmt_scanning  = true;
    g_bmt_prov_mode = BMT_PROV_MODE_MANUAL;
    printf("\n[SCAN] Scanning %ds...\n", BMT_SCAN_DURATION_MS/1000);
    vTaskDelay(pdMS_TO_TICKS(BMT_SCAN_DURATION_MS));
    g_bmt_scanning = false;
    printf("[SCAN] Done.\n");
    bmt_print_scan_list();
}

static void bmt_provision_scan_list(void)
{
    int count = 0;
    for (int i = 0; i < g_bmt_scan_count; i++) {
        if (bmt_uuid_already_provisioned(g_bmt_scan_list[i].uuid)) {
            printf("[PROV] Skip [%d] already provisioned\n", i);
            continue;
        }
        esp_ble_mesh_unprov_dev_add_t dev = {0};
        memcpy(dev.uuid, g_bmt_scan_list[i].uuid, 16);
        memcpy(dev.addr, g_bmt_scan_list[i].addr,  6);
        dev.addr_type = g_bmt_scan_list[i].addr_type;
        dev.oob_info  = g_bmt_scan_list[i].oob_info;
        dev.bearer    = ESP_BLE_MESH_PROV_ADV;
        esp_err_t err = esp_ble_mesh_provisioner_add_unprov_dev(
            &dev, ADD_DEV_FLUSHABLE_DEV_FLAG | ADD_DEV_START_PROV_NOW_FLAG);
        if (err == ESP_OK) {
            printf("[PROV] Provisioning [%d] %s...\n",
                   i, bmt_uuid_type_str(g_bmt_scan_list[i].uuid));
            count++;
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            printf("[PROV] Failed [%d]: %s\n", i, esp_err_to_name(err));
        }
    }
    if (count == 0) printf("[PROV] Nothing to provision.\n");
}

static void bmt_print_status(void)
{
    printf("\n=========== GATEWAY COMMANDS ===========\n");
    printf("1 -> LIST PROVISIONED NODES\n");
    printf("s -> SCAN BEACONS (%ds)\n", BMT_SCAN_DURATION_MS/1000);
    printf("p -> PROVISION SCAN LIST\n");
    printf("a -> AUTO PROVISION MODE\n");
    printf("4 -> SHOW STATUS\n");
    printf("0 -> CLEAR NVS (forget all nodes)\n");
    printf("Provision mode: %s\n",
           g_bmt_prov_mode == BMT_PROV_MODE_AUTO ? "AUTO" : "MANUAL");
    printf("=========================================\n");
}

/* ============================================================================
 * WIFI
 * ============================================================================ */
static void bmt_wifi_event_handler(void *arg, esp_event_base_t base,
                                   int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
        xEventGroupClearBits(g_bmt_wifi_evgrp, BMT_WIFI_CONNECTED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi connected! IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(g_bmt_wifi_evgrp, BMT_WIFI_CONNECTED_BIT);
    }
}

static void bmt_wifi_init(void)
{
    g_bmt_wifi_evgrp = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_t any_id, got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &bmt_wifi_event_handler, NULL, &any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &bmt_wifi_event_handler, NULL, &got_ip));
    wifi_config_t wifi_cfg = { .sta = { .threshold.authmode = WIFI_AUTH_WPA2_PSK } };
    strncpy((char *)wifi_cfg.sta.ssid,     BMT_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strncpy((char *)wifi_cfg.sta.password, BMT_WIFI_PASS, sizeof(wifi_cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    xEventGroupWaitBits(g_bmt_wifi_evgrp, BMT_WIFI_CONNECTED_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);
}

/* ============================================================================
 * MQTT (ThingsBoard Gateway API)
 * ============================================================================ */
static void bmt_mqtt_event_handler(void *args, esp_event_base_t base,
                                   int32_t id, void *data)
{
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        g_bmt_mqtt_connected = true;
        ESP_LOGI(TAG, "MQTT connected to ThingsBoard");
        break;
    case MQTT_EVENT_DISCONNECTED:
        g_bmt_mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    default: break;
    }
}

static void bmt_mqtt_init(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri    = BMT_TB_HOST,
        .credentials.username  = BMT_TB_GATEWAY_TOKEN,  /* ThingsBoard: token = username */
    };
    g_bmt_mqtt_client = esp_mqtt_client_init(&cfg);
    if (!g_bmt_mqtt_client) {
        ESP_LOGE(TAG, "MQTT init failed");
        return;
    }
    esp_mqtt_client_register_event(g_bmt_mqtt_client, ESP_EVENT_ANY_ID,
                                   bmt_mqtt_event_handler, NULL);
    esp_mqtt_client_start(g_bmt_mqtt_client);
}

/* ============================================================================
 * THINGSBOARD GATEWAY API PRIMITIVES
 *
 * Reference: https://thingsboard.io/docs/reference/gateway-mqtt-api/
 *
 *   v1/gateway/connect       → auto-provision sub-device
 *   v1/gateway/disconnect    → mark sub-device offline
 *   v1/gateway/telemetry     → batched telemetry cho nhiều sub-device
 *   v1/gateway/attributes    → batched attributes cho nhiều sub-device
 *   v1/devices/me/telemetry  → telemetry của chính gateway
 *   v1/devices/me/attributes → attributes của chính gateway
 * ============================================================================ */
static void bmt_tb_connect_device(const char *device_name)
{
    if (!g_bmt_mqtt_connected || !g_bmt_mqtt_client) return;
    char json[64];
    snprintf(json, sizeof(json), "{\"device\":\"%s\"}", device_name);
    esp_mqtt_client_publish(g_bmt_mqtt_client, "v1/gateway/connect", json, 0, 1, 0);
    ESP_LOGI(TAG, "TB CONNECT: %s", device_name);
}

static void bmt_tb_disconnect_device(const char *device_name)
{
    if (!g_bmt_mqtt_connected || !g_bmt_mqtt_client) return;
    char json[64];
    snprintf(json, sizeof(json), "{\"device\":\"%s\"}", device_name);
    esp_mqtt_client_publish(g_bmt_mqtt_client, "v1/gateway/disconnect", json, 0, 1, 0);
    ESP_LOGI(TAG, "TB DISCONNECT: %s", device_name);
}

static void bmt_tb_set_role(const char *device_name, const char *role)
{
    if (!g_bmt_mqtt_connected || !g_bmt_mqtt_client) return;
    char json[128];
    snprintf(json, sizeof(json),
             "{\"%s\":{\"role\":\"%s\"}}",
             device_name, role);
    esp_mqtt_client_publish(g_bmt_mqtt_client, "v1/gateway/attributes", json, 0, 1, 0);
    ESP_LOGI(TAG, "TB ATTR [%s]: role=%s", device_name, role);
}

/* ============================================================================
 * MQTT PUBLISH (high-level)
 * ============================================================================ */
static void bmt_mqtt_pub_gateway_online(void)
{
    if (!g_bmt_mqtt_connected || !g_bmt_mqtt_client) return;

    /* Telemetry: status */
    esp_mqtt_client_publish(g_bmt_mqtt_client, "v1/devices/me/telemetry",
                            "{\"status\":\"ONLINE\"}", 0, 1, 0);

    /* Attribute: role */
    esp_mqtt_client_publish(g_bmt_mqtt_client, "v1/devices/me/attributes",
                            "{\"role\":\"" BMT_ROLE_GATEWAY "\"}", 0, 1, 0);

    ESP_LOGI(TAG, "TB Gateway ONLINE (role=" BMT_ROLE_GATEWAY ")");
}

/* Publish status cho node (relay HOẶC scan — phân biệt qua param `role`) */
static void bmt_mqtt_pub_node_status(uint16_t addr, const char *role, bool online)
{
    if (!g_bmt_mqtt_connected || !g_bmt_mqtt_client) return;
    char dev[32], json[192];
    snprintf(dev, sizeof(dev), BMT_DEV_NAME_NODE_FMT, addr);

    if (online) {
        bmt_tb_connect_device(dev);
        bmt_tb_set_role(dev, role);
    }

    snprintf(json, sizeof(json),
             "{\"%s\":[{\"status\":\"%s\",\"addr\":\"0x%04x\"}]}",
             dev, online ? "ONLINE" : "OFFLINE", addr);
    esp_mqtt_client_publish(g_bmt_mqtt_client, "v1/gateway/telemetry", json, 0, 1, 0);
    ESP_LOGI(TAG, "TB TELEMETRY [%s]: %s", dev, json);

    if (!online) bmt_tb_disconnect_device(dev);
}

static void bmt_mqtt_pub_tag(bmt_tag_report_t *r)
{
    if (!g_bmt_mqtt_connected || !g_bmt_mqtt_client || !r) return;
    char dev[32], json[256];
    float distance_m = r->distance_dm / 10.0f;

    snprintf(dev, sizeof(dev), BMT_DEV_NAME_TAG_FMT, r->tag_id);

    /* Auto-connect lần đầu thấy tag (TB sẽ tự tạo sub-device) */
    static uint16_t s_seen_tags[BMT_MAX_SEEN_TAGS] = {0};
    static int      s_seen_count = 0;
    bool first_seen = true;
    for (int i = 0; i < s_seen_count; i++) {
        if (s_seen_tags[i] == r->tag_id) { first_seen = false; break; }
    }
    if (first_seen && s_seen_count < BMT_MAX_SEEN_TAGS) {
        s_seen_tags[s_seen_count++] = r->tag_id;
        bmt_tb_connect_device(dev);
        bmt_tb_set_role(dev, BMT_ROLE_TAG);
    }

    snprintf(json, sizeof(json),
             "{\"%s\":[{"
             "\"scanner\":\"0x%02x\","
             "\"type\":\"%s\","
             "\"rssi\":%d,"
             "\"distance\":%.2f,"
             "\"loss\":%u"
             "}]}",
             dev,
             r->scanner_id,
             r->tag_type == 0x01 ? "PERSON" : "ASSET",
             r->rssi, distance_m, r->loss_pct);
    esp_mqtt_client_publish(g_bmt_mqtt_client, "v1/gateway/telemetry", json, 0, 1, 0);
    ESP_LOGI(TAG, "TB TELEMETRY [%s]: %s", dev, json);
}

/* ============================================================================
 * UART CMD
 * ============================================================================ */
static void bmt_uart_init(void)
{
    const uart_config_t cfg = {
        .baud_rate  = BMT_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(BMT_UART_NUM,
                    BMT_UART_RX_BUF_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(BMT_UART_NUM, &cfg));
}

static void bmt_uart_cmd_task(void *arg)
{
    uint8_t ch;
    (void)arg;
    bmt_print_status();

    while (1) {
        int len = uart_read_bytes(BMT_UART_NUM, &ch, 1, pdMS_TO_TICKS(200));
        if (len <= 0 || ch == '\r' || ch == '\n') continue;

        switch (ch) {
        case '1':
            bmt_log_node_table();
            break;
        case 's':
            printf("\n[UART] Starting MANUAL SCAN...\n");
            bmt_do_scan();
            break;
        case 'p':
            if (g_bmt_prov_mode != BMT_PROV_MODE_MANUAL)
                printf("\n[UART] Not in MANUAL mode. Press s first.\n");
            else
                bmt_provision_scan_list();
            break;
        case 'a':
            g_bmt_prov_mode  = BMT_PROV_MODE_AUTO;
            g_bmt_scan_count = 0;
            memset(g_bmt_scan_list, 0, sizeof(g_bmt_scan_list));
            printf("\n[UART] AUTO provision mode\n");
            break;
        case '4':
            bmt_print_status();
            break;
        case '0':
            printf("\n[UART] Clearing NVS + REBOOT...\n");
            /* Quan trọng: phải xóa nodes khỏi mesh stack state (NVS riêng) trước,
             * không chỉ xóa g_bmt_nodes app-level. Nếu không, mesh stack vẫn nhớ
             * UUID cũ đã gắn với unicast addr → re-provision cùng node sẽ
             * "Timeout, giving up transaction" vì state conflict. */
            {
                const esp_ble_mesh_node_t **entry =
                    esp_ble_mesh_provisioner_get_node_table_entry();
                if (entry) {
                    int erased = 0;
                    for (int i = 0; i < CONFIG_BLE_MESH_MAX_PROV_NODES; i++) {
                        if (entry[i]) {
                            esp_ble_mesh_provisioner_delete_node_with_uuid(entry[i]->dev_uuid);
                            erased++;
                        }
                    }
                    printf("[UART] Erased %d node(s) from mesh stack\n", erased);
                }
            }
            bmt_nvs_clear_nodes();
            memset(g_bmt_nodes, 0, sizeof(g_bmt_nodes));
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
            break;
        default:
            printf("\n[UART] Unknown: %c\n", ch);
            bmt_print_status();
            break;
        }
    }
}

/* ============================================================================
 * SCAN NODE CONFIG TASK
 *
 * Mục đích: gửi APP_KEY_ADD rồi mới MODEL_APP_BIND cho scan node
 *
 * Tại sao 2 bước theo thứ tự này:
 *   1. APP_KEY_ADD     → scan node lưu AppKey vào local list
 *                      → mesh_config_server_cb trên scan node fires
 *                      → g_app_idx được set → scan node có thể publish
 *   2. MODEL_APP_BIND  → bind AppKey vào Vendor Model
 *                      → model mới dùng được AppKey để send/recv
 *
 * Tại sao dùng task riêng (không inline trong callback):
 *   - Mesh callback chạy trong mesh internal task
 *   - vTaskDelay trong callback sẽ block toàn bộ mesh stack
 *   → Task riêng để delay an toàn
 * ============================================================================ */
static void bmt_scan_config_task(void *arg)
{
    uint16_t addr = (uint16_t)(uint32_t)arg;
    ESP_LOGI(TAG, "[SCN_CFG] Configuring scan node 0x%04x...", addr);

    vTaskDelay(pdMS_TO_TICKS(2000));

    /* -------- STEP 1: APP_KEY_ADD -------- */
    {
        esp_ble_mesh_client_common_param_t  c = {0};
        esp_ble_mesh_cfg_client_set_state_t s = {0};
        c.opcode              = ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD;
        c.model               = &bmt_root_models[1];
        c.ctx.net_idx         = g_bmt_net_key_idx;
        c.ctx.app_idx         = 0xFFFF;
        c.ctx.addr            = addr;
        c.ctx.send_ttl        = 7;
        c.msg_timeout         = 8000;
        s.app_key_add.net_idx = g_bmt_net_key_idx;
        s.app_key_add.app_idx = g_bmt_app_key_idx;
        memcpy(s.app_key_add.app_key, g_bmt_app_key, 16);

        esp_err_t e = esp_ble_mesh_config_client_set_state(&c, &s);
        if (e == ESP_OK)
            ESP_LOGI(TAG, "[SCN_CFG] Step 1: APP_KEY_ADD sent to 0x%04x", addr);
        else
            ESP_LOGE(TAG, "[SCN_CFG] Step 1: APP_KEY_ADD FAILED: %s", esp_err_to_name(e));
    }

    vTaskDelay(pdMS_TO_TICKS(3000));

    /* -------- STEP 2: MODEL_APP_BIND -------- */
    {
        esp_ble_mesh_client_common_param_t  c = {0};
        esp_ble_mesh_cfg_client_set_state_t s = {0};
        c.opcode                       = ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND;
        c.model                        = &bmt_root_models[1];
        c.ctx.net_idx                  = g_bmt_net_key_idx;
        c.ctx.app_idx                  = 0xFFFF;
        c.ctx.addr                     = addr;
        c.ctx.send_ttl                 = 7;
        c.msg_timeout                  = 5000;
        s.model_app_bind.element_addr  = addr;
        s.model_app_bind.model_app_idx = g_bmt_app_key_idx;
        s.model_app_bind.model_id      = BMT_VND_MODEL_ID;
        s.model_app_bind.company_id    = BMT_CID_ESP;

        esp_err_t e = esp_ble_mesh_config_client_set_state(&c, &s);
        if (e == ESP_OK)
            ESP_LOGI(TAG, "[SCN_CFG] Step 2: MODEL_APP_BIND sent to 0x%04x", addr);
        else
            ESP_LOGE(TAG, "[SCN_CFG] Step 2: MODEL_APP_BIND FAILED: %s", esp_err_to_name(e));
    }

    vTaskDelay(pdMS_TO_TICKS(2000));

    int idx = bmt_find_node_index(addr);
    if (idx >= 0) {
        g_bmt_nodes[idx].config_done = true;
        bmt_nvs_save_nodes();
    }
    ESP_LOGI(TAG, "[SCN_CFG] Scan node 0x%04x fully configured!", addr);
    bmt_log_node_table();

    vTaskDelete(NULL);
}

/* ============================================================================
 * MESH CALLBACKS
 * ============================================================================ */
static void bmt_mesh_prov_cb(esp_ble_mesh_prov_cb_event_t event,
                             esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_PROV_REGISTER_COMP_EVT:
        ESP_LOGI(TAG, "Provisioner registered");
        break;

    case ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(TAG, "Provisioner scan enabled");
        /* AppKey bind đã xử lý trong bmt_ble_mesh_init_gateway()
         * bằng cách gán trực tiếp bmt_vnd_models[0].keys[0] = g_bmt_app_key_idx */
        break;

    case ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT: {
        const uint8_t *uuid      = param->provisioner_recv_unprov_adv_pkt.dev_uuid;
        const uint8_t *mac       = param->provisioner_recv_unprov_adv_pkt.addr;
        uint8_t        addr_type = param->provisioner_recv_unprov_adv_pkt.addr_type;
        uint16_t       oob_info  = param->provisioner_recv_unprov_adv_pkt.oob_info;

        bmt_mac_cache_store(uuid, mac);

        if (g_bmt_prov_mode == BMT_PROV_MODE_MANUAL) {
            if (!g_bmt_scanning) break;
            for (int i = 0; i < g_bmt_scan_count; i++)
                if (memcmp(g_bmt_scan_list[i].uuid, uuid, 16) == 0) goto scan_dup;
            if (g_bmt_scan_count < BMT_MAX_SCAN_LIST) {
                memcpy(g_bmt_scan_list[g_bmt_scan_count].uuid, uuid, 16);
                memcpy(g_bmt_scan_list[g_bmt_scan_count].addr, mac,  6);
                g_bmt_scan_list[g_bmt_scan_count].addr_type = addr_type;
                g_bmt_scan_list[g_bmt_scan_count].oob_info  = oob_info;
                g_bmt_scan_list[g_bmt_scan_count].used      = true;
                g_bmt_scan_count++;
                printf("[SCAN] [%d] %-7s MAC:",
                       g_bmt_scan_count-1, bmt_uuid_type_str(uuid));
                for (int b = 0; b < 6; b++) printf("%02X%s", mac[b], b<5?":":"");
                printf("\n");
            }
            scan_dup: break;
        }

        if (bmt_uuid_already_provisioned(uuid)) break;

        ESP_LOGI(TAG, "Found unprovisioned [%s]", bmt_uuid_type_str(uuid));
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
        ESP_LOGI(TAG, "Provision complete addr=0x%04x type=%s",
                 addr, bmt_uuid_type_str(uuid));

        uint8_t mac[6] = {0};
        bmt_mac_cache_get(uuid, mac);

        int idx = bmt_add_node(addr, uuid, mac, NULL);
        if (idx < 0) { ESP_LOGW(TAG, "Node table full"); break; }

        /* RELAY: không cần AppKey bind */
        if (bmt_uuid_is_relay(uuid)) {
            g_bmt_nodes[idx].is_relay     = true;
            g_bmt_nodes[idx].is_scan      = false;
            g_bmt_nodes[idx].online       = false;
            g_bmt_nodes[idx].last_seen_ms = 0;
            g_bmt_nodes[idx].config_done  = true;
            snprintf(g_bmt_nodes[idx].name, sizeof(g_bmt_nodes[idx].name),
                     "Relay_0x%04x", addr);
            ESP_LOGI(TAG, "Node 0x%04x = RELAY", addr);
            bmt_nvs_save_nodes();
            bmt_log_node_table();
            break;
        }

        /* SCAN: APP_KEY_ADD → MODEL_APP_BIND qua task riêng */
        if (bmt_uuid_is_scan(uuid)) {
            g_bmt_nodes[idx].is_scan     = true;
            g_bmt_nodes[idx].is_relay    = false;
            g_bmt_nodes[idx].config_done = false;
            snprintf(g_bmt_nodes[idx].name, sizeof(g_bmt_nodes[idx].name),
                     "Scan_0x%04x", addr);
            ESP_LOGI(TAG, "Node 0x%04x = SCAN, launching config task...", addr);
            bmt_nvs_save_nodes();
            bmt_log_node_table();

            xTaskCreate(bmt_scan_config_task, "scan_cfg", 3072,
                        (void *)(uint32_t)addr, 5, NULL);
            break;
        }

        ESP_LOGW(TAG, "Unknown node type");
        bmt_nvs_save_nodes();
        bmt_log_node_table();
        break;
    }

    default: break;
    }
}

static void bmt_mesh_cfg_client_cb(esp_ble_mesh_cfg_client_cb_event_t event,
                                   esp_ble_mesh_cfg_client_cb_param_t *param)
{
    if (!param || !param->params) return;
    uint16_t addr = param->params->ctx.addr;
    int      idx  = bmt_find_node_index(addr);

    /* ACK từ các bước config scan */
    if (event == ESP_BLE_MESH_CFG_CLIENT_SET_STATE_EVT) {
        uint32_t opcode = param->params->opcode;

        if (opcode == ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD) {
            ESP_LOGI(TAG, "[CFG] APP_KEY_ADD ACK from 0x%04x — AppKey delivered!", addr);
        }

        if (opcode == ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND) {
            ESP_LOGI(TAG, "[CFG] MODEL_APP_BIND ACK from 0x%04x", addr);
            if (idx >= 0 && g_bmt_nodes[idx].is_scan) {
                ESP_LOGI(TAG, "=== Scan node 0x%04x READY — will publish tag data ===", addr);

                /* Publish scan node ONLINE lên ThingsBoard */
                bmt_mqtt_pub_node_status(addr, BMT_ROLE_SCAN, true);
            }
        }

        if (opcode == ESP_BLE_MESH_MODEL_OP_RELAY_SET) {
            ESP_LOGI(TAG, "[CFG] RELAY_SET ACK from 0x%04x", addr);
        }
    }

    if (event == ESP_BLE_MESH_CFG_CLIENT_TIMEOUT_EVT) {
        uint32_t opcode = param->params->opcode;
        ESP_LOGW(TAG,
                 "[CFG] TIMEOUT opcode=0x%04" PRIx32 " addr=0x%04x — retransmit or check mesh coverage",
                 opcode, addr);
    }

    /* Reply từ ping relay (GET_STATE) */
    if (event == ESP_BLE_MESH_CFG_CLIENT_GET_STATE_EVT) {
        if (idx >= 0 && g_bmt_nodes[idx].is_relay) {
            g_bmt_nodes[idx].last_seen_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (!g_bmt_nodes[idx].online) {
                g_bmt_nodes[idx].online = true;
                ESP_LOGI(TAG, "Relay 0x%04x ONLINE", addr);
                bmt_mqtt_pub_node_status(addr, BMT_ROLE_RELAY, true);
            }
        }
    }
}

static void bmt_mesh_cfg_server_cb(esp_ble_mesh_cfg_server_cb_event_t event,
                                   esp_ble_mesh_cfg_server_cb_param_t *param)
{
    (void)param;
    ESP_LOGI(TAG, "Config server event: %d", event);
}

/* ============================================================================
 * VENDOR MODEL CALLBACK (nhận tag report từ Scan Node)
 * ============================================================================ */
static void bmt_mesh_vnd_client_cb(esp_ble_mesh_model_cb_event_t event,
                                   esp_ble_mesh_model_cb_param_t *param)
{
    ESP_LOGI(TAG, "[VND] Callback event=%d", event);

    if (event != ESP_BLE_MESH_MODEL_OPERATION_EVT) return;
    if (!param) return;

    uint32_t opcode = param->model_operation.opcode;
    uint16_t src    = param->model_operation.ctx->addr;
    uint8_t *data   = param->model_operation.msg;
    uint16_t len    = param->model_operation.length;

    ESP_LOGI(TAG, "[VND] Received opcode=0x%06" PRIx32 " src=0x%04x len=%d",
             opcode, src, len);

    if (opcode != BMT_OP_VND_TAG_STATUS) return;
    if (len < sizeof(bmt_tag_report_t)) {
        ESP_LOGW(TAG, "[VND] Payload too short: %d < %d",
                 len, (int)sizeof(bmt_tag_report_t));
        return;
    }

    bmt_tag_report_t report;
    memcpy(&report, data, sizeof(report));

    float distance_m = report.distance_dm / 10.0f;
    ESP_LOGI(TAG,
             "[VND] Tag: scanner=0x%02x tag=0x%04x type=%s rssi=%ddBm dist=%.2fm loss=%u%%",
             report.scanner_id, report.tag_id,
             report.tag_type == 0x01 ? "PERSON" : "ASSET",
             report.rssi, distance_m, report.loss_pct);

    bmt_mqtt_pub_tag(&report);
}

/* ============================================================================
 * TASKS
 * ============================================================================ */
static void bmt_relay_ping_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(15000));

    while (1) {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        for (int i = 0; i < BMT_MAX_NODES; i++) {
            if (!g_bmt_nodes[i].used || !g_bmt_nodes[i].is_relay) continue;

            esp_ble_mesh_client_common_param_t  common = {0};
            esp_ble_mesh_cfg_client_get_state_t get    = {0};
            common.opcode       = ESP_BLE_MESH_MODEL_OP_DEFAULT_TTL_GET;
            common.model        = &bmt_root_models[1];
            common.ctx.net_idx  = g_bmt_net_key_idx;
            common.ctx.app_idx  = 0xFFFF;
            common.ctx.addr     = g_bmt_nodes[i].addr;
            common.ctx.send_ttl = 7;
            common.msg_timeout  = 5000;
            esp_err_t err = esp_ble_mesh_config_client_get_state(&common, &get);
            if (err != ESP_OK)
                ESP_LOGW(TAG, "Ping relay 0x%04X failed", g_bmt_nodes[i].addr);

            if (g_bmt_nodes[i].last_seen_ms > 0 &&
                (now - g_bmt_nodes[i].last_seen_ms) > BMT_RELAY_OFFLINE_TIMEOUT_MS) {
                if (g_bmt_nodes[i].online) {
                    g_bmt_nodes[i].online = false;
                    ESP_LOGW(TAG, "Relay 0x%04X OFFLINE", g_bmt_nodes[i].addr);
                    bmt_mqtt_pub_node_status(g_bmt_nodes[i].addr,
                                             BMT_ROLE_RELAY, false);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        vTaskDelay(pdMS_TO_TICKS(BMT_RELAY_PING_INTERVAL_MS));
    }
}

static void bmt_wdt_feed_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    while (1) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

/* ============================================================================
 * MESH INIT
 * ============================================================================ */
static esp_err_t bmt_ble_mesh_init_gateway(void)
{
    esp_err_t err;

    esp_ble_mesh_register_prov_callback(bmt_mesh_prov_cb);
    esp_ble_mesh_register_config_client_callback(bmt_mesh_cfg_client_cb);
    esp_ble_mesh_register_config_server_callback(bmt_mesh_cfg_server_cb);
    esp_ble_mesh_register_custom_model_callback(bmt_mesh_vnd_client_cb);

    err = esp_ble_mesh_init(&bmt_provision, &bmt_composition);
    if (err != ESP_OK) return err;

    err = esp_ble_mesh_provisioner_set_dev_uuid_match(NULL, 0, 0, false);
    if (err != ESP_OK) return err;

    err = esp_ble_mesh_provisioner_prov_enable(
        ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
    if (err != ESP_OK) return err;

    /* Add NetKey */
    const uint8_t *exist_net =
        esp_ble_mesh_provisioner_get_local_net_key(g_bmt_net_key_idx);
    if (!exist_net) {
        err = esp_ble_mesh_provisioner_add_local_net_key(g_bmt_net_key, g_bmt_net_key_idx);
        if (err != ESP_OK) return err;
        ESP_LOGI(TAG, "NetKey added");
    }

    /* Add AppKey */
    const uint8_t *exist_app =
        esp_ble_mesh_provisioner_get_local_app_key(g_bmt_net_key_idx, g_bmt_app_key_idx);
    if (!exist_app) {
        err = esp_ble_mesh_provisioner_add_local_app_key(
            g_bmt_app_key, g_bmt_net_key_idx, g_bmt_app_key_idx);
        if (err != ESP_OK) return err;
        ESP_LOGI(TAG, "AppKey added");
    }

    /* FIX: Gán AppKey vào vendor model trực tiếp qua keys[] array
     *
     * Vấn đề: esp_ble_mesh_provisioner_bind_app_key_to_local_model() fail trong
     * ESP-IDF v6.0 với lỗi "No model found, model id 0x0000, cid 0x0000"
     *
     * Workaround: gán trực tiếp vào model->keys[0]
     *   - bmt_vnd_models[0].keys[i] init = 0xFFFF (ESP_BLE_MESH_KEY_UNUSED)
     *   - Stack check keys[] khi nhận message với AppKey
     *   - keys[0] = 0x0000 → match → deliver lên application callback */
    bmt_vnd_models[0].keys[0] = g_bmt_app_key_idx;
    ESP_LOGI(TAG, "Gateway vendor model AppKey bound directly: keys[0]=0x%04x",
             g_bmt_app_key_idx);

    ESP_LOGI(TAG, "BLE Mesh Gateway init OK");
    return ESP_OK;
}

/* ============================================================================
 * MAIN
 * ============================================================================ */
void app_main(void)
{
    esp_err_t err;
    ESP_LOGI(TAG, "=== BMT Gateway (BLE Mesh ↔ ThingsBoard) Starting ===");

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    bmt_nvs_load_nodes();

    bmt_wifi_init();
    bmt_mqtt_init();
    vTaskDelay(pdMS_TO_TICKS(2000));

    err = bluetooth_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BT init failed: %s", esp_err_to_name(err)); return;
    }

    err = bmt_ble_mesh_init_gateway();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Mesh init failed: %s", esp_err_to_name(err)); return;
    }

    bmt_uart_init();

    printf("\n================ BMT GATEWAY ================\n");
    bmt_print_hex_key("NetKey: ", g_bmt_net_key, 16);
    bmt_print_hex_key("AppKey: ", g_bmt_app_key, 16);
    printf("TB     : %s (ThingsBoard)\n", BMT_TB_HOST);
    printf("Device : %s\n", BMT_DEV_NAME_GATEWAY);
    printf("=============================================\n");

    bmt_print_status();
    bmt_log_node_table();
    bmt_mqtt_pub_gateway_online();

    /* FIX WDT: cấu hình WDT TRƯỚC khi tạo wdt_feed_task
     * Không gọi esp_task_wdt_add(NULL) ở đây — main_task sẽ return ngay
     * → main_task bị xóa → WDT vẫn monitor task đã chết → crash sau 60s */
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = BMT_WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    esp_task_wdt_reconfigure(&wdt_cfg);

    xTaskCreate(bmt_uart_cmd_task,   "bmt_uart",       4096, NULL, 4, NULL);
    xTaskCreate(bmt_relay_ping_task, "bmt_relay_ping", 4096, NULL, 3, NULL);
    xTaskCreate(bmt_wdt_feed_task,   "bmt_wdt_feed",   2048, NULL, 2, NULL);

    ESP_LOGI(TAG, "=== BMT Gateway READY ===");
}