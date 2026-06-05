#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_gap_ble_api.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "driver/uart.h"

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_config_model_api.h"

#include "ble_mesh_example_init.h"
#include "esp_ble_mesh_generic_model_api.h"

/* ==========================================================================
 * VENDOR MODEL DEFINITIONS
 * CID = 0x02E5 (Espressif)
 * Model ID = 0x0000 (custom tracking)
 *
 * Payload struct (mesh_tag_report_t) — FIX: giảm xuống 8 bytes
 * scanner_id(1) + tag_type(1) + tag_id(2) + rssi(1) + distance_dm(2) + loss_pct(1) = 8 bytes
 * 8 bytes + opcode 3 bytes = 11 bytes = MAX unsegmented BLE Mesh PDU
 * ========================================================================== */
#define CID_ESP                     0x02E5
#define ESP_BLE_MESH_VND_MODEL_ID   0x0000

#define OP_VND_TAG_STATUS   ESP_BLE_MESH_MODEL_OP_3(0x00, CID_ESP)

#pragma pack(1)
/* FIX: 8 bytes thay vì 13 bytes → UNSEGMENTED → không cần ACK
 * 13 bytes cũ → segmented (2 PDU) → cần ACK → "Ran out of retransmit attempts"
 * 8 bytes mới → 1 PDU → fire-and-forget → không còn retransmit issue */
typedef struct {
    uint8_t  scanner_id;    /* 1B */
    uint8_t  tag_type;      /* 1B */
    uint16_t tag_id;        /* 2B */
    int8_t   rssi;          /* 1B: RSSI filtered (dBm) */
    int16_t  distance_dm;   /* 2B: khoảng cách × 10 dm (chia 10 = mét) */
    uint8_t  loss_pct;      /* 1B: loss rate % 0-100 */
} mesh_tag_report_t;        /* = 8 bytes → UNSEGMENTED ✓ */
#pragma pack()

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define TAG "SCANNER"

/* ==========================================================================
 * TU BUILD - CONSTANTS
 * ========================================================================== */
#define TAG_MAGIC               0xAB
#define TAG_TYPE_PERSON         0x01
#define TAG_TYPE_ASSET          0x02
#define COMPANY_ID_LOW          0xE5
#define COMPANY_ID_HIGH         0x02
#define MAX_TAGS                20
#define TAG_TIMEOUT_MS          5000
#define SCANNER_ID              0x01

#define LOG_RSSI_THRESHOLD_DBM  3
#define LOG_MIN_INTERVAL_MS     2000

/* =========================================================
 * TU BUILD - TIME DIVISION RADIO MANAGEMENT
 *
 * 1 ESP32 chỉ có 1 radio:
 * Mesh scan và GAP scan conflict nếu chạy cùng lúc
 *
 * Giải pháp: Time Division TỰ ĐỘNG
 * Chu kỳ ~1500ms:
 * |<-- GAP scan 1000ms -->|<-- Mesh publish 500ms -->|
 *    Bắt ADV tag              Gửi data lên mesh
 *    Kalman filter
 *    Update loss rate
 *
 * GAP scan 1000ms:
 * Tag ADV interval 450-550ms
 * 1000ms bắt được ~2 ADV, đủ cho Kalman hội tụ
 *
 * Mesh 500ms:
 * Packet nhỏ, mesh flood nhanh
 * ========================================================= */
#define GAP_SCAN_DURATION_MS      1000   /* scan tag 1 giây */
#define MESH_PUBLISH_DURATION_MS   500   /* gửi mesh 0.5 giây */

/* 100% duty cycle trong GAP scan phase */
#define SCAN_INTERVAL_UNITS     0x0010  /* 10ms */
#define SCAN_WINDOW_UNITS       0x0010  /* 10ms = 100% */

#define PROV_COMPLETE_BIT       BIT0

typedef enum {
    PHASE_GAP_SCAN = 0,
    PHASE_MESH_PUB = 1,
} radio_phase_t;

static volatile radio_phase_t g_phase        = PHASE_GAP_SCAN;
static volatile bool          g_has_new_data = false;

/* ==========================================================================
 * TU BUILD - PAYLOAD STRUCT
 * ========================================================================== */
#pragma pack(1)
typedef struct {
    uint8_t  magic;
    uint8_t  tag_type;
    uint16_t tag_id;
    int8_t   tx_power;
    uint8_t  sequence;
    uint16_t crc16;
} tag_payload_t;
#pragma pack()

/* ==========================================================================
 * TU BUILD - TAG TRACKING TABLE
 * ========================================================================== */
typedef struct {
    bool     active;
    uint8_t  tag_type;
    uint16_t tag_id;
    int8_t   tx_power;
    int8_t   rssi_raw;
    float    rssi_filtered;
    float    distance;
    uint8_t  last_sequence;
    uint32_t total_received;
    uint32_t total_missed;
    uint32_t last_seen_ms;
    uint8_t  mac[6];
    int8_t   last_logged_rssi;
    uint32_t last_log_ms;
} tag_info_t;

static tag_info_t g_tags[MAX_TAGS];

/* ==========================================================================
 * TU BUILD - KALMAN FILTER
 * ========================================================================== */
typedef struct {
    float q, r, x, p, k;
} kalman_t;

static kalman_t g_kalman[MAX_TAGS];

static void kalman_init(kalman_t *kf, float initial_rssi)
{
    kf->q = 0.1f; kf->r = 2.0f;
    kf->x = initial_rssi; kf->p = 1.0f; kf->k = 0.0f;
}

static float kalman_update(kalman_t *kf, float rssi)
{
    kf->p = kf->p + kf->q;
    kf->k = kf->p / (kf->p + kf->r);
    kf->x = kf->x + kf->k * (rssi - kf->x);
    kf->p = (1.0f - kf->k) * kf->p;
    return kf->x;
}

/* ==========================================================================
 * TU BUILD - CRC-16 CCITT
 * ========================================================================== */
static uint16_t crc16_ccitt(uint8_t *data, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else              crc <<= 1;
        }
    }
    return crc;
}

static bool verify_crc16(uint8_t *data, int len, uint16_t received_crc)
{
    return (crc16_ccitt(data, len) == received_crc);
}

/* ==========================================================================
 * TU BUILD - DISTANCE CALCULATION
 * Path loss model: distance = 10^((tx_power - rssi) / (10*n))
 * n = 2.5 cho indoor
 * ========================================================================== */
#define PATH_LOSS_N 2.5f

static float calculate_distance(int8_t tx_power, float rssi_filtered)
{
    if (rssi_filtered >= 0) return 0.0f;
    float ratio = (tx_power - rssi_filtered) / (10.0f * PATH_LOSS_N);
    return powf(10.0f, ratio);
}

/* ==========================================================================
 * TU BUILD - TAG TABLE MANAGEMENT
 * ========================================================================== */
static int find_tag(uint16_t tag_id)
{
    for (int i = 0; i < MAX_TAGS; i++)
        if (g_tags[i].active && g_tags[i].tag_id == tag_id) return i;
    return -1;
}

static int add_tag(uint16_t tag_id, uint8_t tag_type,
                   int8_t tx_power, int8_t rssi,
                   uint8_t sequence, uint8_t *mac)
{
    for (int i = 0; i < MAX_TAGS; i++) {
        if (!g_tags[i].active) {
            memset(&g_tags[i], 0, sizeof(tag_info_t));
            g_tags[i].active           = true;
            g_tags[i].tag_type         = tag_type;
            g_tags[i].tag_id           = tag_id;
            g_tags[i].tx_power         = tx_power;
            g_tags[i].rssi_raw         = rssi;
            g_tags[i].rssi_filtered    = (float)rssi;
            g_tags[i].last_sequence    = sequence;
            g_tags[i].total_received   = 1;
            g_tags[i].last_seen_ms     = xTaskGetTickCount() * portTICK_PERIOD_MS;
            g_tags[i].last_logged_rssi = rssi;
            g_tags[i].last_log_ms      = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (mac) memcpy(g_tags[i].mac, mac, 6);
            kalman_init(&g_kalman[i], (float)rssi);
            g_tags[i].distance = calculate_distance(tx_power, (float)rssi);
            ESP_LOGI(TAG, "New tag: 0x%04X (%s) RSSI=%ddBm Dist=%.2fm",
                     tag_id, tag_type == TAG_TYPE_PERSON ? "PERSON" : "ASSET",
                     rssi, g_tags[i].distance);
            return i;
        }
    }
    ESP_LOGW(TAG, "Tag table full!");
    return -1;
}

static void update_tag(int idx, int8_t rssi, uint8_t sequence)
{
    tag_info_t *t   = &g_tags[idx];
    uint32_t    now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (sequence == t->last_sequence) {
        t->rssi_raw      = rssi;
        t->rssi_filtered = kalman_update(&g_kalman[idx], (float)rssi);
        t->distance      = calculate_distance(t->tx_power, t->rssi_filtered);
        t->last_seen_ms  = now;
        t->total_received++;
        int8_t   rd = (int8_t)abs((int)rssi - (int)t->last_logged_rssi);
        uint32_t td = now - t->last_log_ms;
        if (rd >= LOG_RSSI_THRESHOLD_DBM || td >= LOG_MIN_INTERVAL_MS) {
            ESP_LOGI(TAG, "Tag 0x%04X | RSSI=%ddBm | Filt=%.1fdBm | Dist=%.2fm | Seq=%d",
                     t->tag_id, rssi, t->rssi_filtered, t->distance, sequence);
            t->last_logged_rssi = rssi; t->last_log_ms = now;
        }
        return;
    }

    int8_t diff = (int8_t)(sequence - t->last_sequence);
    if (diff < 0 && diff > -10) {
        ESP_LOGD(TAG, "Tag 0x%04X backward skip", t->tag_id); return;
    }

    uint8_t expected = t->last_sequence + 1;
    if (sequence != expected) {
        uint8_t missed = 0;
        if (diff > 1)        missed = (uint8_t)(diff - 1);
        else if (diff < -10) missed = (uint8_t)(256 - t->last_sequence - 1 + sequence);
        if (missed > 0 && missed < 200) {
            t->total_missed += missed;
            ESP_LOGW(TAG, "Tag 0x%04X miss %d exp=%d got=%d",
                     t->tag_id, missed, expected, sequence);
        }
    }

    t->rssi_raw      = rssi;
    t->rssi_filtered = kalman_update(&g_kalman[idx], (float)rssi);
    t->distance      = calculate_distance(t->tx_power, t->rssi_filtered);
    t->last_sequence  = sequence;
    t->total_received++;
    t->last_seen_ms   = now;
    g_has_new_data    = true;

    int8_t   rd = (int8_t)abs((int)rssi - (int)t->last_logged_rssi);
    uint32_t td = now - t->last_log_ms;
    if (rd >= LOG_RSSI_THRESHOLD_DBM || td >= LOG_MIN_INTERVAL_MS) {
        ESP_LOGI(TAG, "Tag 0x%04X | RSSI=%ddBm | Filt=%.1fdBm | Dist=%.2fm | Seq=%d",
                 t->tag_id, rssi, t->rssi_filtered, t->distance, sequence);
        t->last_logged_rssi = rssi; t->last_log_ms = now;
    }
}

/* ==========================================================================
 * TU BUILD - PRINT TAG TABLE
 * ========================================================================== */
static void print_tag_table(void)
{
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    bool has_tag = false;
    printf("\n========== TAG TABLE (Scanner 0x%02X) ==========\n", SCANNER_ID);
    for (int i = 0; i < MAX_TAGS; i++) {
        if (!g_tags[i].active) continue;
        has_tag = true;
        tag_info_t *t   = &g_tags[i];
        uint32_t    age = (now - t->last_seen_ms) / 1000;
        float    lr  = 0.0f;
        uint32_t tot = t->total_received + t->total_missed;
        if (tot > 0) lr = (float)t->total_missed / tot * 100.0f;
        printf("Tag 0x%04X:\n", t->tag_id);
        printf("  Type      : %s\n", t->tag_type == TAG_TYPE_PERSON ? "PERSON" : "ASSET");
        printf("  MAC       : %02X:%02X:%02X:%02X:%02X:%02X\n",
               t->mac[0], t->mac[1], t->mac[2], t->mac[3], t->mac[4], t->mac[5]);
        printf("  RSSI raw  : %d dBm\n",   t->rssi_raw);
        printf("  RSSI filt : %.1f dBm\n", t->rssi_filtered);
        printf("  Distance  : %.2f m\n",   t->distance);
        printf("  TX Power  : %d dBm\n",   t->tx_power);
        printf("  Sequence  : %d\n",       t->last_sequence);
        printf("  Received  : %lu\n",      t->total_received);
        printf("  Missed    : %lu\n",      t->total_missed);
        printf("  Loss rate : %.1f%%\n",   lr);
        printf("  Last seen : %lus ago\n", age);
        printf("--------------------------------------------------\n");
    }
    if (!has_tag) {
        printf("  No tags in range\n");
        printf("--------------------------------------------------\n");
    }
}

/* ==========================================================================
 * TU BUILD - PARSE ADV PAYLOAD
 * ========================================================================== */
static bool parse_tag_payload(uint8_t *adv_data, uint8_t adv_len,
                               tag_payload_t *out)
{
    if (!adv_data || adv_len < 4 || !out) return false;
    int pos = 0;
    while (pos < adv_len) {
        uint8_t len  = adv_data[pos];
        if (len == 0 || pos + len >= adv_len) break;
        uint8_t type = adv_data[pos + 1];
        
        // 0x03 là mã của 16-bit Service UUID
        if (type == 0x03 && len >= 3) {
            uint16_t uuid = adv_data[pos + 2] | (adv_data[pos + 3] << 8);
            
            // Nếu UUID khớp với 0x180D (Heart Rate - iPhone rất thích phát cái này)
            if (uuid == 0x180D) {
                out->magic    = 0xAB; // Giả lập các giá trị cố định vì UUID không chứa data thô
                out->tag_type = 0x01;
                out->tag_id   = 0x0001;
                out->tx_power = 0;
                out->sequence = 0;
                out->crc16    = 0;
                return true;
            }
        }
        pos += len + 1;
    }
    return false;
}

/* ==========================================================================
 * BLE MESH - SCANNER NODE
 * UUID = "SCAN" (0x53,0x43,0x41,0x4E)
 * Gateway nhận diện bằng uuid_is_scanner()
 * byte cuối đổi cho mỗi scanner: 0x01, 0x02, 0x03...
 * ========================================================================== */
static uint8_t dev_uuid[16] = {
    0x53, 0x43, 0x41, 0x4E, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01
};

static uint16_t g_node_addr = 0x0000;
static uint16_t g_net_idx   = 0xFFFF;
static uint16_t g_app_idx   = 0xFFFF;

static esp_ble_mesh_cfg_srv_t config_server = {
    .net_transmit     = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay            = ESP_BLE_MESH_RELAY_ENABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon           = ESP_BLE_MESH_BEACON_ENABLED,
    .gatt_proxy       = ESP_BLE_MESH_GATT_PROXY_ENABLED,
    .friend_state     = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .default_ttl      = 7,
};

ESP_BLE_MESH_MODEL_PUB_DEFINE(vnd_pub, sizeof(mesh_tag_report_t) + 4, ROLE_NODE);

static esp_ble_mesh_model_op_t vnd_ops[] = {
    ESP_BLE_MESH_MODEL_OP(OP_VND_TAG_STATUS, sizeof(mesh_tag_report_t)),
    ESP_BLE_MESH_MODEL_OP_END,
};

static esp_ble_mesh_model_t vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(CID_ESP, ESP_BLE_MESH_VND_MODEL_ID,
                              vnd_ops, &vnd_pub, NULL),
};

static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
};

static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models, vnd_models),
};

static esp_ble_mesh_comp_t composition = {
    .cid           = 0x02E5,
    .element_count = ARRAY_SIZE(elements),
    .elements      = elements,
};

static esp_ble_mesh_prov_t provision = { .uuid = dev_uuid };

static EventGroupHandle_t g_mesh_event_group;

/* ==========================================================================
 * DUNG API - SCAN PARAMS
 * ========================================================================== */
static esp_ble_scan_params_t g_scan_params = {
    .scan_type          = BLE_SCAN_TYPE_PASSIVE,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval      = SCAN_INTERVAL_UNITS,
    .scan_window        = SCAN_WINDOW_UNITS,
    .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
};

/* ==========================================================================
 * DUNG API - GAP CALLBACK
 * ========================================================================== */
static void gap_event_handler(esp_gap_ble_cb_event_t event,
                               esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "Scan params set OK");
        break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS)
            ESP_LOGD(TAG, "[Phase 1] GAP scan running...");
        else
            ESP_LOGE(TAG, "Scan start FAILED");
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) break;

        /* Chỉ xử lý tag khi đã provision xong và đang ở Phase GAP scan */
        if (g_phase != PHASE_GAP_SCAN) break;
        EventBits_t bits = xEventGroupGetBits(g_mesh_event_group);
        if (!(bits & PROV_COMPLETE_BIT)) break;

        uint8_t *adv_data = param->scan_rst.ble_adv;
        uint8_t  adv_len  = param->scan_rst.adv_data_len;
        int8_t   rssi     = param->scan_rst.rssi;
        uint8_t *mac      = param->scan_rst.bda;
        tag_payload_t payload;
        if (!parse_tag_payload(adv_data, adv_len, &payload)) break;
        int idx = find_tag(payload.tag_id);
        if (idx < 0) {
            idx = add_tag(payload.tag_id, payload.tag_type,
                          payload.tx_power, rssi, payload.sequence, mac);
        } else {
            update_tag(idx, rssi, payload.sequence);
        } 
        
        g_has_new_data = true; 
        
        break;
    }
    default: break;
    }
}

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
        ESP_LOGI(TAG, "Waiting for AppKey from Gateway...");
        /* g_app_idx vẫn là 0xFFFF, sẽ được set khi Gateway gửi APP_KEY_ADD */
        xEventGroupSetBits(g_mesh_event_group, PROV_COMPLETE_BIT);
        break;

    case ESP_BLE_MESH_NODE_PROV_RESET_EVT:
        ESP_LOGI(TAG, "Node reset -> unprovisioned");
        g_node_addr = 0x0000; g_net_idx = 0xFFFF; g_app_idx = 0xFFFF;
        esp_ble_mesh_node_prov_enable(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT);
        break;

    default: break;
    }
}

static void mesh_config_server_cb(esp_ble_mesh_cfg_server_cb_event_t event,
                                  esp_ble_mesh_cfg_server_cb_param_t *param)
{
    if (event != ESP_BLE_MESH_CFG_SERVER_STATE_CHANGE_EVT) return;

    switch (param->ctx.recv_op) {
    case ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD:
        /* FIX: đây là bước quan trọng nhất
         * Gateway gửi APP_KEY_ADD → scanner lưu AppKey
         * g_app_idx được set → publish_tags_to_mesh() mới chạy được
         * Nếu không có bước này: g_app_idx mãi là 0xFFFF → publish bị block */
        g_app_idx = param->value.state_change.appkey_add.app_idx;
        ESP_LOGI(TAG, "=== AppKey received! idx=0x%04x — Scanner ready to publish! ===",
                 g_app_idx);
        break;
    case ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND:
        ESP_LOGI(TAG, "Model AppKey bind done — Vendor Model fully configured");
        break;
    case ESP_BLE_MESH_MODEL_OP_RELAY_SET:
        ESP_LOGI(TAG, "Relay state changed");
        break;
    case ESP_BLE_MESH_MODEL_OP_MODEL_PUB_SET:
        ESP_LOGI(TAG, "Model publication set");
        break;
    default:
        break;
    }
}

/* ==========================================================================
 * TU BUILD - TASKS
 * ========================================================================== */

/* ==========================================================================
 * TU BUILD - PUBLISH TAG DATA LEN MESH
 *
 * FIX 1: Tách log "not provisioned" vs "waiting AppKey"
 *   Trước đây: cả 2 trường hợp đều in "Not provisioned"
 *              → khó biết thực sự đang ở trạng thái nào
 *   Bây giờ: phân biệt rõ ràng để debug
 *
 * FIX 2: Bỏ pre-fill pub->msg trước khi gọi esp_ble_mesh_model_publish
 *   Vấn đề: esp_ble_mesh_model_publish() bên trong gọi bt_mesh_model_msg_init()
 *           → RESET pub->msg → data đã fill trước bị mất
 *           → Chỉ cần pass data trực tiếp vào esp_ble_mesh_model_publish
 * ========================================================================== */
static void publish_tags_to_mesh(void)
{
    /* FIX: phân biệt 2 trường hợp khác nhau */
    if (g_node_addr == 0x0000) {
        ESP_LOGW(TAG, "[Mesh] Not provisioned yet, skip publish");
        return;
    }
    if (g_app_idx == 0xFFFF) {
        /* Trạng thái này ĐÚNG khi vừa provision xong
         * nhưng Gateway chưa gửi APP_KEY_ADD tới
         * Sau khi Gateway gửi APP_KEY_ADD → g_app_idx được set
         * → publish sẽ tự động chạy ở cycle tiếp theo */
        ESP_LOGI(TAG, "[Mesh] Provisioned (addr=0x%04x) but waiting for AppKey...",
                 g_node_addr);
        return;
    }

    int published = 0;
    for (int i = 0; i < MAX_TAGS; i++) {
        if (!g_tags[i].active) continue;

        /* Tính loss rate */
        uint32_t total    = g_tags[i].total_received + g_tags[i].total_missed;
        uint8_t  loss_pct = 0;
        if (total > 0)
            loss_pct = (uint8_t)((g_tags[i].total_missed * 100) / total);

        /* FIX: dùng int16_t distance_dm thay vì float distance
         * Tránh segmentation (13B → 8B) → không còn "Ran out of retransmit"
         * Encode: distance (m) × 10 = dm, gateway decode: dm ÷ 10 = m */
        int16_t distance_dm = (int16_t)(g_tags[i].distance * 10.0f);
        if (distance_dm < 0) distance_dm = 0;

        mesh_tag_report_t report = {
            .scanner_id  = SCANNER_ID,
            .tag_type    = g_tags[i].tag_type,
            .tag_id      = g_tags[i].tag_id,
            .rssi        = (int8_t)g_tags[i].rssi_filtered,
            .distance_dm = distance_dm,
            .loss_pct    = loss_pct,
        };

        /* Set publish parameters */
        vnd_models[0].pub->publish_addr = 0x0001;  /* Gateway unicast addr */
        vnd_models[0].pub->app_idx      = g_app_idx;
        vnd_models[0].pub->ttl          = 7;

        esp_err_t err = esp_ble_mesh_model_publish(
            &vnd_models[0],
            OP_VND_TAG_STATUS,
            sizeof(report),
            (uint8_t *)&report,
            ROLE_NODE);

        if (err == ESP_OK) {
            published++;
            ESP_LOGI(TAG, "[Mesh] Published tag 0x%04X RSSI=%ddBm Dist=%.1fm Loss=%u%%",
                     report.tag_id, report.rssi,
                     distance_dm / 10.0f, loss_pct);
        } else {
            ESP_LOGW(TAG, "[Mesh] Publish tag 0x%04X failed: %s",
                     g_tags[i].tag_id, esp_err_to_name(err));
        }

        /* Delay nhỏ giữa mỗi tag tránh flood mesh */
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (published == 0)
        ESP_LOGD(TAG, "[Mesh] No active tags to publish");
    else
        ESP_LOGI(TAG, "[Mesh] Published %d tag(s) to mesh", published);
}

/* =========================================================
 * Task chính: Time Division Radio Management
 *
 * Sau khi provision xong:
 * Lặp lại chu kỳ:
 *   Phase 1 (1000ms): GAP scan tag
 *   Phase 2 (500ms):  Stop GAP scan → Publish → Resume mesh
 * ========================================================= */
static void radio_manager_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Waiting for mesh provision...");
    xEventGroupWaitBits(g_mesh_event_group, PROV_COMPLETE_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    ESP_LOGI(TAG, "Mesh provisioned, waiting to stabilize...");
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* Set scan params một lần duy nhất */
    esp_ble_gap_stop_scanning();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_err_t err = esp_ble_gap_set_scan_params(&g_scan_params);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_scan_params failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "=== Time Division Radio Manager started ===");
    ESP_LOGI(TAG, "GAP scan: %dms | Mesh publish: %dms | Cycle: %dms",
             GAP_SCAN_DURATION_MS, MESH_PUBLISH_DURATION_MS,
             GAP_SCAN_DURATION_MS + MESH_PUBLISH_DURATION_MS);

    g_phase = PHASE_MESH_PUB;

    while (1) {
        /* ---- PHASE 1: GAP SCAN ---- */
        g_phase = PHASE_GAP_SCAN;
        g_has_new_data = false;

        ESP_LOGD(TAG, "[Phase 1] GAP scan %dms", GAP_SCAN_DURATION_MS);
        esp_ble_gap_start_scanning(0);
        vTaskDelay(pdMS_TO_TICKS(GAP_SCAN_DURATION_MS));

        /* ---- PHASE 2: MESH PUBLISH ---- */
        g_phase = PHASE_MESH_PUB;
        esp_ble_gap_stop_scanning();
        vTaskDelay(pdMS_TO_TICKS(100));

        if (g_has_new_data) {
            ESP_LOGI(TAG, "[Phase 2] Publishing tag data to mesh...");
            publish_tags_to_mesh();
            print_tag_table();
            g_has_new_data = false;
        }

        vTaskDelay(pdMS_TO_TICKS(MESH_PUBLISH_DURATION_MS - 100));
    }
}

static void timeout_check_task(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        for (int i = 0; i < MAX_TAGS; i++) {
            if (!g_tags[i].active) continue;
            if ((now - g_tags[i].last_seen_ms) > TAG_TIMEOUT_MS) {
                ESP_LOGW(TAG, "Tag 0x%04X OUT OF RANGE", g_tags[i].tag_id);
                g_tags[i].active = false;
                memset(&g_kalman[i], 0, sizeof(kalman_t));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ==========================================================================
 * BLE MESH INIT
 * ========================================================================== */
static esp_err_t ble_mesh_init_scanner(void)
{
    esp_ble_mesh_register_prov_callback(mesh_prov_cb);
    esp_ble_mesh_register_config_server_callback(mesh_config_server_cb);

    ESP_ERROR_CHECK(esp_ble_mesh_init(&provision, &composition));

    bool provisioned = esp_ble_mesh_node_is_provisioned();

    if (provisioned) {
        /* Đã provision rồi (NVS còn lưu)
         * PROV_COMPLETE_EVT KHÔNG trigger lại khi reboot
         * Set bit ngay để radio_manager_task chạy luôn */
        ESP_LOGI(TAG, "Already provisioned (restored from NVS)");

        /* g_node_addr chỉ dùng để check != 0x0000 trong publish_tags_to_mesh()
         * esp_ble_mesh_get_primary_element_address() không có trong ESP-IDF cũ
         * → dùng placeholder 0x0001 (giá trị bất kỳ != 0 là đủ)
         * Giá trị chính xác được set trong PROV_COMPLETE_EVT lần đầu provision */
        g_node_addr = 0x0001;
        ESP_LOGI(TAG, "Node addr restored (placeholder=0x%04x)", g_node_addr);

        /* Dùng app_idx mặc định 0x0000 — Gateway luôn dùng app_key_idx = 0x0000
         * AppKey và model binding đã được lưu trong mesh NVS,
         * stack tự restore → publish sẽ hoạt động ngay */
        g_app_idx = 0x0000;
        ESP_LOGI(TAG, "app_idx=0x%04x — ready to publish", g_app_idx);

        xEventGroupSetBits(g_mesh_event_group, PROV_COMPLETE_BIT);
    } else {
        /* Chưa provision:
         * Giữ phase = MESH_PUB để mesh scan chạy
         * → Gateway thấy scanner beacon → provision
         * Sau khi provision xong:
         *   → PROV_COMPLETE_EVT → set bit → radio_manager_task bắt đầu
         *   → Gateway gửi APP_KEY_ADD → g_app_idx set
         *   → Gateway gửi MODEL_APP_BIND → model ready
         *   → publish_tags_to_mesh() sẽ chạy được */
        g_phase = PHASE_MESH_PUB;
        ESP_ERROR_CHECK(esp_ble_mesh_node_prov_enable(
            ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT));
        ESP_LOGI(TAG, "BLE Mesh Scanner initialized");
        ESP_LOGI(TAG, "UUID: SCAN (53:43:41:4E:...:01)");
        ESP_LOGI(TAG, "Waiting for Gateway to provision...");
    }

    return ESP_OK;
}

/* ==========================================================================
 * UART COMMAND TASK
 * Lệnh debug qua serial (MobaXterm):
 *   r → Reset mesh provision (xóa NVS mesh, về unprovisioned)
 *       Dùng khi Gateway bị clear/reflash mà scanner vẫn còn NVS cũ
 *   1 → In trạng thái hiện tại
 * ========================================================================== */
static void uart_cmd_task(void *arg)
{
    (void)arg;

    /* Khởi tạo UART0 đọc từ serial monitor */
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

    printf("\n===== SCANNER UART COMMANDS =====\n");
    printf("r -> RESET mesh (xoa NVS, ve unprovisioned)\n");
    printf("1 -> STATUS hien tai\n");
    printf("=================================\n");

    uint8_t ch;
    while (1) {
        int len = uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(200));
        if (len <= 0 || ch == '\r' || ch == '\n') continue;

        switch (ch) {
        case 'r':
        case 'R':
            printf("\n[UART] Resetting mesh provision...\n");
            printf("[UART] Scanner se ve unprovisioned va broadcast beacon moi\n");
            printf("[UART] Gateway can scan lai de provision\n\n");
            /* Reset mesh: xóa NVS provision, trở về unprovisioned state
             * Sau khi gọi hàm này → NODE_PROV_RESET_EVT fires
             * → mesh_prov_cb enable lại provisioning beacon */
            esp_ble_mesh_node_local_reset();
            g_node_addr = 0x0000;
            g_net_idx   = 0xFFFF;
            g_app_idx   = 0xFFFF;
            break;

        case '1':
            printf("\n===== SCANNER STATUS =====\n");
            printf("Node addr : 0x%04X %s\n", g_node_addr,
                   g_node_addr == 0 ? "(NOT provisioned)" : "(provisioned)");
            printf("Net idx   : 0x%04X\n", g_net_idx);
            printf("App idx   : 0x%04X %s\n", g_app_idx,
                   g_app_idx == 0xFFFF ? "(waiting AppKey)" : "(AppKey OK)");
            printf("Phase     : %s\n",
                   g_phase == PHASE_GAP_SCAN ? "GAP_SCAN" : "MESH_PUB");
            printf("==========================\n");
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
    ESP_LOGI(TAG, "=== BLE Scanner + Mesh Node Starting ===");
    ESP_LOGI(TAG, "Scanner ID: 0x%02X", SCANNER_ID);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    g_mesh_event_group = xEventGroupCreate();

    err = bluetooth_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "BT init failed!"); return; }

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));

    err = ble_mesh_init_scanner();
    if (err != ESP_OK) { ESP_LOGE(TAG, "Mesh init failed!"); return; }

    xTaskCreate(radio_manager_task,  "radio_mgr",     3072, NULL, 6, NULL);
    xTaskCreate(timeout_check_task,  "timeout_check", 2048, NULL, 3, NULL);
    xTaskCreate(uart_cmd_task,       "uart_cmd",      2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "=== SCANNER READY (r=reset mesh, 1=status) ===");
}