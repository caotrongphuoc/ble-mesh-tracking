/* ============================================================================
 * BMT (BLE Mesh Tracking) — GATEWAY firmware  [v3 — MQTT Queue Worker]
 * ----------------------------------------------------------------------------
 * Role  : Provisioner + BLE Mesh ↔ ThingsBoard MQTT bridge + Zone Detection
 * Board : ESP32 DevKitC WROOM-32
 * IDF   : v6.0
 *
 * What's new in v3 (vs v2):
 *   - Tách MQTT publish ra task riêng (queue-based worker)
 *   - Mesh VND callback chỉ enqueue, không block
 *   - Fix bug: khi MQTT disconnect/slow, mesh task không bị nghẽn nữa
 *     → Gateway không drop PDU từ scan_2/scan_3 khi đang xử lý scan_1
 *
 * Architecture:
 *   mesh VND cb ──► xQueueSend(g_bmt_mqtt_queue, report)  (non-blocking)
 *                            │
 *                            ▼
 *                   bmt_mqtt_worker_task ──► esp_mqtt_client_publish
 *                            (chuyên xử lý MQTT, có thể block thoải mái)
 * ============================================================================ */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include <limits.h>

#include "esp_log.h"
#include "esp_bt.h"           /* TX power control */
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
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

#define BMT_TB_HOST                     "mqtt://192.168.1.113:1883"
#define BMT_TB_GATEWAY_TOKEN            "Sxj62zRhBtUj3tZIRdhd"

/* Device naming */
#define BMT_DEV_NAME_GATEWAY            "bmt_gateway"
#define BMT_DEV_NAME_NODE_FMT           "bmt_node_0x%04x"
#define BMT_DEV_NAME_TAG_FMT            "bmt_tag_0x%04x"

/* Role attribute values */
#define BMT_ROLE_GATEWAY                "gateway"
#define BMT_ROLE_RELAY                  "relay"
#define BMT_ROLE_SCAN                   "scan"
#define BMT_ROLE_TAG                    "tag"

/* ============================================================================
 * ZONE DETECTION CONFIG
 * ============================================================================ */
#define BMT_MAX_SCANNERS                8
#define BMT_MAX_TRACKED_TAGS            16
#define BMT_ZONE_HYSTERESIS_DBM         5
#define BMT_SCANNER_VALID_MS            3500
#define BMT_TAG_OUT_OF_RANGE_MS         10000
#define BMT_ZONE_UNKNOWN                0xFF

/* ============================================================================
 * MQTT QUEUE CONFIG  [v3]
 *
 * Mesh VND callback chỉ enqueue tag report. Worker task tách riêng publish MQTT.
 * → Mesh task không block bởi MQTT slow/disconnect.
 *
 * Queue size 64: với 3 scanner publish 1 report/1.5s = 2 report/s/scanner = 6/s tổng.
 * 64 slot = ~10s buffer. Đủ để xử lý burst khi MQTT chậm tạm thời.
 * ============================================================================ */
#define BMT_MQTT_QUEUE_SIZE             64
#define BMT_MQTT_PUBLISH_TIMEOUT_MS     500   /* timeout per publish call */

/* ============================================================================
 * BLE MESH VENDOR MODEL
 * ============================================================================ */
#define BMT_CID_ESP                     0x02E5
#define BMT_VND_MODEL_ID                0x0000
#define BMT_OP_VND_TAG_STATUS           ESP_BLE_MESH_MODEL_OP_3(0x00, BMT_CID_ESP)
#define BMT_OP_VND_NODE_HEALTH          ESP_BLE_MESH_MODEL_OP_3(0x02, BMT_CID_ESP)  /* [NEW] */

#pragma pack(1)
typedef struct {
    uint8_t  scanner_id;
    uint8_t  tag_type;
    uint16_t tag_id;
    int8_t   rssi;
    int16_t  distance_dm;
    uint8_t  loss_pct;
} bmt_tag_report_t;         /* 8 bytes — UNSEGMENTED */
#pragma pack()

/* [NEW] Node health report — Gateway nhận từ scan node mỗi 30s */
#pragma pack(1)
typedef struct {
    uint8_t  scanner_id;
    int8_t   chip_temp_c;
    uint16_t vdd_mv;
    uint16_t free_heap_kb;
    uint16_t uptime_min;
} bmt_node_health_t;        /* 8 bytes — UNSEGMENTED */
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

#define BMT_NVS_NAMESPACE               "bmt_gw"
#define BMT_NVS_KEY_NODES               "node_table"

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

typedef struct {
    bool     active;
    uint16_t tag_id;
    uint8_t  tag_type;
    int8_t   rssi_by_scanner [BMT_MAX_SCANNERS];
    uint32_t ts_by_scanner   [BMT_MAX_SCANNERS];
    bool     valid_by_scanner[BMT_MAX_SCANNERS];
    uint8_t  current_zone_id;
    uint32_t last_zone_change_ms;
    uint32_t last_any_report_ms;
} bmt_gateway_tag_track_t;

/* ============================================================================
 * GLOBALS
 * ============================================================================ */
static bmt_gateway_mac_cache_t   g_bmt_mac_cache[BMT_MAX_MAC_CACHE];
static bmt_gateway_node_t        g_bmt_nodes[BMT_MAX_NODES];
static bmt_gateway_scan_entry_t  g_bmt_scan_list[BMT_MAX_SCAN_LIST];
static bmt_gateway_tag_track_t   g_bmt_tag_track[BMT_MAX_TRACKED_TAGS];

static int                       g_bmt_scan_count = 0;
static bool                      g_bmt_scanning   = false;
static bmt_gateway_prov_mode_t   g_bmt_prov_mode  = BMT_PROV_MODE_MANUAL;

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

static EventGroupHandle_t       g_bmt_wifi_evgrp;
static const int                BMT_WIFI_CONNECTED_BIT = BIT0;
static esp_mqtt_client_handle_t g_bmt_mqtt_client    = NULL;
static bool                     g_bmt_mqtt_connected = false;

/* [v3] MQTT queue — non-blocking enqueue từ mesh callback */
static QueueHandle_t            g_bmt_mqtt_queue = NULL;

/* Counter để track queue stats */
static uint32_t                 g_bmt_mqtt_enqueued = 0;
static uint32_t                 g_bmt_mqtt_dropped  = 0;
static uint32_t                 g_bmt_mqtt_published = 0;

/* Mesh models — MAX retransmit + Friend enable (theo nguyên tắc max optimize) */
static esp_ble_mesh_cfg_srv_t bmt_cfg_server = {
    .net_transmit     = ESP_BLE_MESH_TRANSMIT(7, 10),    /* max 8 lần, interval 10ms */
    .relay            = ESP_BLE_MESH_RELAY_ENABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(7, 10),    /* forward mạnh nhất */
    .beacon           = ESP_BLE_MESH_BEACON_ENABLED,
    .gatt_proxy       = ESP_BLE_MESH_GATT_PROXY_ENABLED,
    .friend_state     = ESP_BLE_MESH_FRIEND_ENABLED,     /* reserved cho tương lai (LPN) */
    .default_ttl      = 7,
};

static esp_ble_mesh_client_t bmt_cfg_client;
static esp_ble_mesh_client_t bmt_vnd_client;

static esp_ble_mesh_model_op_t bmt_vnd_ops[] = {
    ESP_BLE_MESH_MODEL_OP(BMT_OP_VND_TAG_STATUS,  sizeof(bmt_tag_report_t)),
    ESP_BLE_MESH_MODEL_OP(BMT_OP_VND_NODE_HEALTH, sizeof(bmt_node_health_t)),
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
    for (int i = 0; i < 16; i++) { printf("%02X", uuid[i]); if (i != 15) printf(":"); }
}

static void bmt_print_mac(const uint8_t *mac)
{
    if (!mac) return;
    for (int i = 0; i < 6; i++) { printf("%02X", mac[i]); if (i != 5) printf(":"); }
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
               g_bmt_nodes[i].is_relay ? "RELAY" :
               g_bmt_nodes[i].is_scan  ? "SCAN"  : "UNKNOWN");
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
    printf("2 -> LIST TRACKED TAGS + ZONES\n");
    printf("3 -> MQTT QUEUE STATS\n");
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
 * ZONE DETECTION
 * ============================================================================ */
static const char *bmt_zone_name(uint8_t scanner_id)
{
    switch (scanner_id) {
    case 0x01: return "bedroom_1";
    case 0x02: return "bedroom_2";
    case 0x03: return "toilet";
    case BMT_ZONE_UNKNOWN: return "out_of_range";
    default:   return "unknown";
    }
}

static bmt_gateway_tag_track_t *bmt_tag_track_find(uint16_t tag_id)
{
    for (int i = 0; i < BMT_MAX_TRACKED_TAGS; i++)
        if (g_bmt_tag_track[i].active && g_bmt_tag_track[i].tag_id == tag_id)
            return &g_bmt_tag_track[i];
    return NULL;
}

static bmt_gateway_tag_track_t *bmt_tag_track_get_or_add(uint16_t tag_id, uint8_t tag_type)
{
    bmt_gateway_tag_track_t *t = bmt_tag_track_find(tag_id);
    if (t) return t;
    for (int i = 0; i < BMT_MAX_TRACKED_TAGS; i++) {
        if (!g_bmt_tag_track[i].active) {
            memset(&g_bmt_tag_track[i], 0, sizeof(g_bmt_tag_track[i]));
            g_bmt_tag_track[i].active          = true;
            g_bmt_tag_track[i].tag_id          = tag_id;
            g_bmt_tag_track[i].tag_type        = tag_type;
            g_bmt_tag_track[i].current_zone_id = BMT_ZONE_UNKNOWN;
            ESP_LOGI(TAG, "New tag tracked: 0x%04x", tag_id);
            return &g_bmt_tag_track[i];
        }
    }
    return NULL;
}

static uint8_t bmt_zone_evaluate(bmt_gateway_tag_track_t *t)
{
    uint32_t now           = xTaskGetTickCount() * portTICK_PERIOD_MS;
    int      best_rssi     = INT_MIN;
    uint8_t  best_scanner  = BMT_ZONE_UNKNOWN;
    int      current_rssi  = INT_MIN;
    bool     current_fresh = false;

    for (int i = 0; i < BMT_MAX_SCANNERS; i++) {
        if (!t->valid_by_scanner[i]) continue;
        if (now - t->ts_by_scanner[i] > BMT_SCANNER_VALID_MS) {
            t->valid_by_scanner[i] = false;
            continue;
        }
        if ((int)t->rssi_by_scanner[i] > best_rssi) {
            best_rssi    = t->rssi_by_scanner[i];
            best_scanner = i + 1;
        }
        if ((i + 1) == t->current_zone_id) {
            current_rssi  = t->rssi_by_scanner[i];
            current_fresh = true;
        }
    }

    if (best_scanner == BMT_ZONE_UNKNOWN) return BMT_ZONE_UNKNOWN;
    if (t->current_zone_id == BMT_ZONE_UNKNOWN || !current_fresh)
        return best_scanner;
    if (best_scanner != t->current_zone_id) {
        if ((best_rssi - current_rssi) < BMT_ZONE_HYSTERESIS_DBM)
            return t->current_zone_id;
    }
    return best_scanner;
}

static void bmt_log_tag_track(void)
{
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    printf("\n========== TRACKED TAGS ==========\n");
    bool any = false;
    for (int i = 0; i < BMT_MAX_TRACKED_TAGS; i++) {
        bmt_gateway_tag_track_t *t = &g_bmt_tag_track[i];
        if (!t->active) continue;
        any = true;
        printf("Tag 0x%04x (type=%s)\n",
               t->tag_id, t->tag_type == 0x01 ? "PERSON" : "ASSET");
        printf("  Zone           : %s (id=0x%02x)\n",
               bmt_zone_name(t->current_zone_id), t->current_zone_id);
        if (t->last_zone_change_ms > 0)
            printf("  Zone since     : %" PRIu32 "s ago\n",
                   (now - t->last_zone_change_ms)/1000);
        printf("  Last report    : %" PRIu32 "s ago\n",
               (now - t->last_any_report_ms)/1000);
        printf("  Scanners seen  :\n");
        bool any_scanner = false;
        for (int j = 0; j < BMT_MAX_SCANNERS; j++) {
            /* Skip scanner chưa bao giờ thấy tag này */
            if (t->ts_by_scanner[j] == 0) continue;
            any_scanner = true;
            uint32_t age = now - t->ts_by_scanner[j];
            const char *status;
            if      (age <= BMT_SCANNER_VALID_MS)    status = "FRESH";
            else if (age <= BMT_TAG_OUT_OF_RANGE_MS) status = "STALE";
            else                                      status = "EXPIRED";
            printf("    0x%02x (%-12s): RSSI=%4d  age=%4us  %s\n",
                   j+1, bmt_zone_name(j+1),
                   t->rssi_by_scanner[j],
                   (unsigned)(age/1000),
                   status);
        }
        if (!any_scanner) printf("    (none seen yet)\n");
        printf("----------------------------------\n");
    }
    if (!any) {
        printf("  No tracked tags\n");
        printf("----------------------------------\n");
    }
}

/* [v3] In stats queue MQTT */
static void bmt_log_mqtt_stats(void)
{
    UBaseType_t in_queue = g_bmt_mqtt_queue ? uxQueueMessagesWaiting(g_bmt_mqtt_queue) : 0;
    printf("\n========== MQTT QUEUE STATS ==========\n");
    printf("Connected       : %s\n", g_bmt_mqtt_connected ? "YES" : "NO");
    printf("Queue size      : %u / %d\n", (unsigned)in_queue, BMT_MQTT_QUEUE_SIZE);
    printf("Total enqueued  : %" PRIu32 "\n", g_bmt_mqtt_enqueued);
    printf("Total published : %" PRIu32 "\n", g_bmt_mqtt_published);
    printf("Total dropped   : %" PRIu32 " (queue full)\n", g_bmt_mqtt_dropped);
    if (g_bmt_mqtt_enqueued > 0) {
        float drop_rate = (float)g_bmt_mqtt_dropped * 100.0f / g_bmt_mqtt_enqueued;
        printf("Drop rate       : %.2f%%\n", drop_rate);
    }
    printf("======================================\n");
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
 * MQTT
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
        .credentials.username  = BMT_TB_GATEWAY_TOKEN,
        /* Network timeout: tránh block quá lâu khi network kém */
        .network.timeout_ms    = BMT_MQTT_PUBLISH_TIMEOUT_MS,
        .network.reconnect_timeout_ms = 5000,
    };
    g_bmt_mqtt_client = esp_mqtt_client_init(&cfg);
    if (!g_bmt_mqtt_client) { ESP_LOGE(TAG, "MQTT init failed"); return; }
    esp_mqtt_client_register_event(g_bmt_mqtt_client, ESP_EVENT_ANY_ID,
                                   bmt_mqtt_event_handler, NULL);
    esp_mqtt_client_start(g_bmt_mqtt_client);
}

/* ============================================================================
 * THINGSBOARD GATEWAY API
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
    snprintf(json, sizeof(json), "{\"%s\":{\"role\":\"%s\"}}", device_name, role);
    esp_mqtt_client_publish(g_bmt_mqtt_client, "v1/gateway/attributes", json, 0, 1, 0);
    ESP_LOGI(TAG, "TB ATTR [%s]: role=%s", device_name, role);
}

/* ============================================================================
 * MQTT PUBLISH (high-level)
 * ============================================================================ */
static void bmt_mqtt_pub_gateway_online(void)
{
    if (!g_bmt_mqtt_connected || !g_bmt_mqtt_client) return;
    esp_mqtt_client_publish(g_bmt_mqtt_client, "v1/devices/me/telemetry",
                            "{\"status\":\"ONLINE\"}", 0, 1, 0);
    esp_mqtt_client_publish(g_bmt_mqtt_client, "v1/devices/me/attributes",
                            "{\"role\":\"" BMT_ROLE_GATEWAY "\"}", 0, 1, 0);
    ESP_LOGI(TAG, "TB Gateway ONLINE (role=" BMT_ROLE_GATEWAY ")");
}

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

/*
 * Tag publish + zone detection — gọi từ worker task, KHÔNG gọi từ mesh callback
 *
 * Update tracking state + evaluate zone + publish MQTT.
 */
static void bmt_mqtt_pub_tag(bmt_tag_report_t *r)
{
    if (!r) return;

    /* ZONE DETECTION: update tracking ngay cả khi MQTT chưa connect — phím 2 vẫn show */
    bmt_gateway_tag_track_t *t = bmt_tag_track_get_or_add(r->tag_id, r->tag_type);
    if (!t) {
        ESP_LOGW(TAG, "Tag track table full, can't track 0x%04x", r->tag_id);
        return;
    }
    if (r->scanner_id < 1 || r->scanner_id > BMT_MAX_SCANNERS) {
        ESP_LOGW(TAG, "Scanner ID 0x%02x out of supported range", r->scanner_id);
        return;
    }
    int sidx = r->scanner_id - 1;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    t->rssi_by_scanner [sidx] = r->rssi;
    t->ts_by_scanner   [sidx] = now;
    t->valid_by_scanner[sidx] = true;
    t->last_any_report_ms     = now;

    uint8_t new_zone = bmt_zone_evaluate(t);
    if (new_zone != t->current_zone_id) {
        ESP_LOGI(TAG, "Tag 0x%04x ZONE CHANGE: %s -> %s",
                 r->tag_id,
                 bmt_zone_name(t->current_zone_id),
                 bmt_zone_name(new_zone));
        t->current_zone_id     = new_zone;
        t->last_zone_change_ms = now;
    }

    /* PUBLISH MQTT — chỉ khi connected */
    if (!g_bmt_mqtt_connected || !g_bmt_mqtt_client) return;

    char dev[32], json[384];
    float distance_m = r->distance_dm / 10.0f;
    snprintf(dev, sizeof(dev), BMT_DEV_NAME_TAG_FMT, r->tag_id);

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
             "\"loss\":%u,"
             "\"zone\":\"%s\","
             "\"zone_id\":\"0x%02x\""
             "}]}",
             dev,
             r->scanner_id,
             r->tag_type == 0x01 ? "PERSON" : "ASSET",
             r->rssi, distance_m, r->loss_pct,
             bmt_zone_name(t->current_zone_id),
             t->current_zone_id);

    int msg_id = esp_mqtt_client_publish(g_bmt_mqtt_client,
                                         "v1/gateway/telemetry",
                                         json, 0, 0, 0);  /* QoS=0 — fire & forget */
    if (msg_id < 0) {
        ESP_LOGW(TAG, "MQTT publish failed for %s", dev);
    } else {
        g_bmt_mqtt_published++;
        ESP_LOGI(TAG, "TB [%s] zone=%s: scanner=0x%02x rssi=%d dist=%.2fm",
                 dev, bmt_zone_name(t->current_zone_id),
                 r->scanner_id, r->rssi, distance_m);
    }
}

/* ============================================================================
 * MQTT WORKER TASK  [v3 — NEW]
 *
 * Đọc bmt_tag_report_t từ queue, publish MQTT. Tách khỏi mesh callback để:
 *   - Mesh callback không block khi MQTT slow/disconnect
 *   - Burst PDU từ 3 scan node được buffer trong queue, drain dần
 * ============================================================================ */
static void bmt_mqtt_worker_task(void *arg)
{
    (void)arg;
    bmt_tag_report_t report;

    ESP_LOGI(TAG, "MQTT worker task started (queue size=%d)", BMT_MQTT_QUEUE_SIZE);

    while (1) {
        if (xQueueReceive(g_bmt_mqtt_queue, &report, portMAX_DELAY) == pdTRUE) {
            bmt_mqtt_pub_tag(&report);
        }
    }
}

/* Out-of-range timeout task */
static void bmt_zone_timeout_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        for (int i = 0; i < BMT_MAX_TRACKED_TAGS; i++) {
            bmt_gateway_tag_track_t *t = &g_bmt_tag_track[i];
            if (!t->active) continue;
            if ((now - t->last_any_report_ms) <= BMT_TAG_OUT_OF_RANGE_MS) continue;
            if (t->current_zone_id == BMT_ZONE_UNKNOWN) continue;

            ESP_LOGW(TAG, "Tag 0x%04x OUT OF RANGE (no report for %us)",
                     t->tag_id,
                     (unsigned int)(BMT_TAG_OUT_OF_RANGE_MS / 1000));
            t->current_zone_id     = BMT_ZONE_UNKNOWN;
            t->last_zone_change_ms = now;
            for (int j = 0; j < BMT_MAX_SCANNERS; j++)
                t->valid_by_scanner[j] = false;

            if (!g_bmt_mqtt_connected || !g_bmt_mqtt_client) continue;
            char dev[32], json[160];
            snprintf(dev, sizeof(dev), BMT_DEV_NAME_TAG_FMT, t->tag_id);
            snprintf(json, sizeof(json),
                     "{\"%s\":[{\"zone\":\"%s\",\"zone_id\":\"0x%02x\"}]}",
                     dev, bmt_zone_name(BMT_ZONE_UNKNOWN), BMT_ZONE_UNKNOWN);
            esp_mqtt_client_publish(g_bmt_mqtt_client, "v1/gateway/telemetry",
                                    json, 0, 0, 0);
            ESP_LOGI(TAG, "TB TELEMETRY [%s]: %s", dev, json);
        }
    }
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
        case '1': bmt_log_node_table(); break;
        case '2': bmt_log_tag_track();  break;
        case '3': bmt_log_mqtt_stats(); break;
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
        case '4': bmt_print_status(); break;
        case '0':
            printf("\n[UART] Clearing NVS + REBOOT...\n");
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
            memset(g_bmt_tag_track, 0, sizeof(g_bmt_tag_track));
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
 * ============================================================================ */
static void bmt_scan_config_task(void *arg)
{
    uint16_t addr = (uint16_t)(uint32_t)arg;
    ESP_LOGI(TAG, "[SCN_CFG] Configuring scan node 0x%04x...", addr);
    vTaskDelay(pdMS_TO_TICKS(2000));

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
        ESP_LOGI(TAG, "[SCN_CFG] Step 1: APP_KEY_ADD to 0x%04x: %s",
                 addr, e == ESP_OK ? "OK" : esp_err_to_name(e));
    }
    vTaskDelay(pdMS_TO_TICKS(3000));
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
        ESP_LOGI(TAG, "[SCN_CFG] Step 2: MODEL_APP_BIND to 0x%04x: %s",
                 addr, e == ESP_OK ? "OK" : esp_err_to_name(e));
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

    if (event == ESP_BLE_MESH_CFG_CLIENT_SET_STATE_EVT) {
        uint32_t opcode = param->params->opcode;
        if (opcode == ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD)
            ESP_LOGI(TAG, "[CFG] APP_KEY_ADD ACK from 0x%04x", addr);
        if (opcode == ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND) {
            ESP_LOGI(TAG, "[CFG] MODEL_APP_BIND ACK from 0x%04x", addr);
            if (idx >= 0 && g_bmt_nodes[idx].is_scan) {
                ESP_LOGI(TAG, "=== Scan node 0x%04x READY ===", addr);
                bmt_mqtt_pub_node_status(addr, BMT_ROLE_SCAN, true);
            }
        }
    }

    if (event == ESP_BLE_MESH_CFG_CLIENT_TIMEOUT_EVT) {
        uint32_t opcode = param->params->opcode;
        ESP_LOGW(TAG, "[CFG] TIMEOUT opcode=0x%04" PRIx32 " addr=0x%04x",
                 opcode, addr);
    }

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
 * VENDOR MODEL CALLBACK  [v3 — non-blocking]
 *
 * Chỉ enqueue tag report vào MQTT queue, KHÔNG gọi publish trực tiếp.
 * Worker task xử lý queue ở background, mesh task quay lại nhận PDU tiếp.
 * ============================================================================ */
static void bmt_mesh_vnd_client_cb(esp_ble_mesh_model_cb_event_t event,
                                   esp_ble_mesh_model_cb_param_t *param)
{
    if (event != ESP_BLE_MESH_MODEL_OPERATION_EVT) return;
    if (!param) return;

    uint32_t opcode = param->model_operation.opcode;
    uint16_t src    = param->model_operation.ctx->addr;
    uint8_t *data   = param->model_operation.msg;
    uint16_t len    = param->model_operation.length;

    /* [NEW] Handle NODE HEALTH report */
    if (opcode == BMT_OP_VND_NODE_HEALTH) {
        if (len < sizeof(bmt_node_health_t)) {
            ESP_LOGW(TAG, "[VND-HEALTH] Payload too short: %d < %d",
                     len, (int)sizeof(bmt_node_health_t));
            return;
        }
        bmt_node_health_t health;
        memcpy(&health, data, sizeof(health));
        ESP_LOGI(TAG,
                 "[VND-HEALTH] src=0x%04x scanner=0x%02x temp=%d°C vdd=%umV heap=%uKB uptime=%umin",
                 src, health.scanner_id, health.chip_temp_c,
                 health.vdd_mv, health.free_heap_kb, health.uptime_min);

        /* Publish lên TB device `bmt_node_0xXXXX` */
        if (g_bmt_mqtt_connected && g_bmt_mqtt_client) {
            char dev[32], json[256];
            snprintf(dev, sizeof(dev), BMT_DEV_NAME_NODE_FMT, src);
            snprintf(json, sizeof(json),
                     "{\"%s\":[{"
                     "\"chip_temp\":%d,"
                     "\"vdd_mv\":%u,"
                     "\"free_heap_kb\":%u,"
                     "\"uptime_min\":%u"
                     "}]}",
                     dev, health.chip_temp_c, health.vdd_mv,
                     health.free_heap_kb, health.uptime_min);
            esp_mqtt_client_publish(g_bmt_mqtt_client,
                                    "v1/gateway/telemetry", json, 0, 0, 0);
            ESP_LOGI(TAG, "TB HEALTH [%s] temp=%d°C vdd=%umV",
                     dev, health.chip_temp_c, health.vdd_mv);
        }
        return;
    }

    if (opcode != BMT_OP_VND_TAG_STATUS) return;
    if (len < sizeof(bmt_tag_report_t)) {
        ESP_LOGW(TAG, "[VND] Payload too short: %d < %d",
                 len, (int)sizeof(bmt_tag_report_t));
        return;
    }

    bmt_tag_report_t report;
    memcpy(&report, data, sizeof(report));

    ESP_LOGI(TAG,
             "[VND] src=0x%04x scanner=0x%02x tag=0x%04x rssi=%ddBm dist=%.2fm loss=%u%%",
             src, report.scanner_id, report.tag_id,
             report.rssi, report.distance_dm / 10.0f, report.loss_pct);

    /* [v3] Enqueue non-blocking — nếu queue đầy thì drop và đếm */
    if (g_bmt_mqtt_queue) {
        if (xQueueSend(g_bmt_mqtt_queue, &report, 0) == pdTRUE) {
            g_bmt_mqtt_enqueued++;
        } else {
            g_bmt_mqtt_dropped++;
            ESP_LOGW(TAG, "MQTT queue FULL — dropped report (total dropped: %" PRIu32 ")",
                     g_bmt_mqtt_dropped);
        }
    }
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
            esp_ble_mesh_config_client_get_state(&common, &get);

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

    const uint8_t *exist_net =
        esp_ble_mesh_provisioner_get_local_net_key(g_bmt_net_key_idx);
    if (!exist_net) {
        err = esp_ble_mesh_provisioner_add_local_net_key(g_bmt_net_key, g_bmt_net_key_idx);
        if (err != ESP_OK) return err;
        ESP_LOGI(TAG, "NetKey added");
    }

    const uint8_t *exist_app =
        esp_ble_mesh_provisioner_get_local_app_key(g_bmt_net_key_idx, g_bmt_app_key_idx);
    if (!exist_app) {
        err = esp_ble_mesh_provisioner_add_local_app_key(
            g_bmt_app_key, g_bmt_net_key_idx, g_bmt_app_key_idx);
        if (err != ESP_OK) return err;
        ESP_LOGI(TAG, "AppKey added");
    }

    bmt_vnd_models[0].keys[0] = g_bmt_app_key_idx;
    ESP_LOGI(TAG, "Gateway vendor model AppKey bound: keys[0]=0x%04x",
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
    ESP_LOGI(TAG, "=== BMT Gateway v3 (MQTT Queue Worker) Starting ===");

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    bmt_nvs_load_nodes();

    /* [v3] Tạo MQTT queue TRƯỚC khi mesh init — nếu mesh callback firing sớm,
     *      queue đã sẵn sàng để enqueue, không null deref */
    g_bmt_mqtt_queue = xQueueCreate(BMT_MQTT_QUEUE_SIZE, sizeof(bmt_tag_report_t));
    if (!g_bmt_mqtt_queue) {
        ESP_LOGE(TAG, "Failed to create MQTT queue!");
        return;
    }
    ESP_LOGI(TAG, "MQTT queue created (size=%d)", BMT_MQTT_QUEUE_SIZE);

    bmt_wifi_init();
    bmt_mqtt_init();
    vTaskDelay(pdMS_TO_TICKS(2000));

    err = bluetooth_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "BT init failed"); return; }

    /* MAX TX POWER
     * ESP32 classic: max +9 dBm (ESP_PWR_LVL_P9)
     * ESP32-S3:      max +20 dBm (ESP_PWR_LVL_P20) — uncomment nếu dùng S3 */
#ifdef CONFIG_IDF_TARGET_ESP32S3
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV,     ESP_PWR_LVL_P20);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN,    ESP_PWR_LVL_P20);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P20);
    ESP_LOGI(TAG, "BLE TX power: +20 dBm (ESP32-S3 long-range)");
#else
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV,     ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN,    ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    ESP_LOGI(TAG, "BLE TX power: +9 dBm (ESP32 classic max)");
#endif

    err = bmt_ble_mesh_init_gateway();
    if (err != ESP_OK) { ESP_LOGE(TAG, "Mesh init failed"); return; }

    bmt_uart_init();

    printf("\n================ BMT GATEWAY v3 ================\n");
    bmt_print_hex_key("NetKey: ", g_bmt_net_key, 16);
    bmt_print_hex_key("AppKey: ", g_bmt_app_key, 16);
    printf("TB     : %s\n", BMT_TB_HOST);
    printf("Device : %s\n", BMT_DEV_NAME_GATEWAY);
    printf("\nZONE MAPPING:\n");
    printf("  scanner 0x01 -> %s\n", bmt_zone_name(0x01));
    printf("  scanner 0x02 -> %s\n", bmt_zone_name(0x02));
    printf("  scanner 0x03 -> %s\n", bmt_zone_name(0x03));
    printf("\nZONE PARAMS:\n");
    printf("  Hysteresis     : %d dBm\n",   BMT_ZONE_HYSTERESIS_DBM);
    printf("  Scanner valid  : %d ms\n",    BMT_SCANNER_VALID_MS);
    printf("  Out-of-range   : %d ms\n",    BMT_TAG_OUT_OF_RANGE_MS);
    printf("\nMQTT QUEUE:\n");
    printf("  Size           : %d slots\n", BMT_MQTT_QUEUE_SIZE);
    printf("================================================\n");

    bmt_print_status();
    bmt_log_node_table();
    bmt_mqtt_pub_gateway_online();

    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = BMT_WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    esp_task_wdt_reconfigure(&wdt_cfg);

    /* [v3] MQTT worker task — priority cao hơn các task khác để drain queue nhanh */
    xTaskCreate(bmt_mqtt_worker_task,  "bmt_mqtt_wkr",   4096, NULL, 5, NULL);
    xTaskCreate(bmt_uart_cmd_task,     "bmt_uart",        4096, NULL, 4, NULL);
    xTaskCreate(bmt_relay_ping_task,   "bmt_relay_ping",  4096, NULL, 3, NULL);
    xTaskCreate(bmt_zone_timeout_task, "bmt_zone_timer",  3072, NULL, 3, NULL);
    xTaskCreate(bmt_wdt_feed_task,     "bmt_wdt_feed",    2048, NULL, 2, NULL);

    ESP_LOGI(TAG, "=== BMT Gateway v3 READY ===");
}