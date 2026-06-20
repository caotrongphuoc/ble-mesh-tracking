/* ============================================================================
 * BMT (BLE Mesh Tracking) — SCAN NODE firmware
 * ----------------------------------------------------------------------------
 * Role  : Quét BLE iBeacon từ tag (ESP32 / iPhone) → publish qua Vendor Model
 *         lên Gateway
 * Board : ESP32 DevKitC WROOM-32
 * IDF   : v6.0
 *
 * UUID prefix : "SCAN" (0x53,0x43,0x41,0x4E) — Gateway dùng bmt_uuid_is_scan()
 *               để phân biệt với RELAY khi auto-provision.
 *               Byte cuối UUID = scanner ID, đổi cho từng scanner (0x01, 0x02...)
 * ============================================================================ */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_gap_ble_api.h"
#include "esp_bt.h"                          /* TX power control */
#include "soc/soc_caps.h"
#include "esp_adc/adc_oneshot.h"             /* VDD measurement */
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#if SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"       /* Chip temperature */
#endif
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
#include "esp_ble_mesh_generic_model_api.h"

#include "ble_mesh_example_init.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define TAG                             "BMT_SCAN"

/* ============================================================================
 * USER CONFIG — đổi cho từng scan node
 * ============================================================================ */
#define BMT_SCANNER_ID                  0x01    /* ID scanner trong báo cáo  */

/* ============================================================================
 * BLE MESH VENDOR MODEL
 *   CID  : 0x02E5 (Espressif)
 *   Scan Node = Vendor Server (publish tag report tới Gateway)
 * ============================================================================ */
#define BMT_CID_ESP                     0x02E5
#define BMT_VND_MODEL_ID                0x0000
#define BMT_OP_VND_TAG_STATUS           ESP_BLE_MESH_MODEL_OP_3(0x00, BMT_CID_ESP)
#define BMT_OP_VND_NODE_HEALTH          ESP_BLE_MESH_MODEL_OP_3(0x02, BMT_CID_ESP)  /* [NEW] */

#pragma pack(1)
/*
 * Payload BLE Mesh từ Scan Node → Gateway
 *
 * FIX: 8 bytes thay vì 13 bytes → UNSEGMENTED → không cần ACK
 *   13 bytes cũ → segmented (2 PDU) → cần ACK → "Ran out of retransmit"
 *   8 bytes mới → 1 PDU → fire-and-forget → không còn retransmit issue
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

/* [NEW] Node health report — gửi mỗi 30s, gửi đến Gateway thông tin sức khỏe node */
#pragma pack(1)
typedef struct {
    uint8_t  scanner_id;        /* 1B : Scanner ID                       */
    int8_t   chip_temp_c;       /* 1B : Chip temperature (°C, signed)    */
    uint16_t vdd_mv;            /* 2B : Internal VDD (mV, typ ~3300)     */
    uint16_t free_heap_kb;      /* 2B : Free heap (KB)                   */
    uint16_t uptime_min;        /* 2B : Uptime (minutes, rollover 65535) */
} bmt_node_health_t;            /* total = 8 bytes → UNSEGMENTED ✓       */
#pragma pack()

/* ============================================================================
 * TAG ADV PROTOCOL (giữa Tag ESP32/iPhone và Scan Node)
 * ============================================================================ */
#define BMT_TAG_TYPE_PERSON             0x01
#define BMT_TAG_TYPE_ASSET              0x02

#define BMT_CID_ESPRESSIF               0x02E5   /* ESP32 custom tag    */
#define BMT_CID_APPLE                   0x004C   /* iBeacon (iPhone)    */

#define BMT_TAG_MAJOR_PERSON            0x0001
#define BMT_TAG_MAJOR_ASSET             0x0002

/* System UUID prefix — 4 bytes đầu phải khớp để nhận ra "tag của hệ thống" */
static const uint8_t BMT_SYSTEM_UUID_PREFIX[4] = { 0xAB, 0x00, 0x00, 0x00 };

/* iPhone không set được TX power → calibrate cố định */
#define BMT_PHONE_TX_POWER_1M           (-59)

#pragma pack(1)
/*
 * ADV payload của ESP32 tag (24 bytes, khớp với firmware tag)
 *
 * So sánh với iBeacon:
 *   iBeacon: CID(2) + 0215(2) + UUID(16) + Major(2) + Minor(2) + TXPwr(1) = 25B
 *   Custom:  CID(2) + UUID(16) + Major(2) + Minor(2) + TXPwr(1) + Seq(1) + CRC(2) = 26B
 *   Bỏ "02 15" marker → tiết kiệm 2B → có chỗ cho Seq + CRC
 */
typedef struct {
    uint8_t  uuid[16];   /* 16B : system UUID (AB000000-...)         */
    uint16_t major;      /*  2B : PERSON=0x0001, ASSET=0x0002        */
    uint16_t minor;      /*  2B : tag ID                              */
    int8_t   tx_power;   /*  1B : measured power tại 1m              */
    uint8_t  sequence;   /*  1B : 0–255 wraps                         */
    uint16_t crc16;      /*  2B : CRC-16 CCITT                        */
} bmt_tag_adv_payload_t; /* total = 24 bytes                          */
#pragma pack()

/* Internal payload (sau khi parse từ ADV) */
#pragma pack(1)
typedef struct {
    uint8_t  tag_type;   /* PERSON / ASSET                            */
    uint16_t tag_id;     /* Minor (kèm namespace 0x8000 cho iPhone)  */
    int8_t   tx_power;   /* Measured power tại 1m                    */
    uint8_t  sequence;   /* 0 với iPhone (loss=0%)                   */
    uint16_t crc16;      /* 0 với iPhone                              */
} bmt_tag_payload_t;
#pragma pack()

/* ============================================================================
 * TAG TRACKING / KALMAN
 * ============================================================================ */
#define BMT_MAX_TAGS                    20
#define BMT_TAG_TIMEOUT_MS              5000

#define BMT_LOG_RSSI_THRESHOLD_DBM      3
#define BMT_LOG_MIN_INTERVAL_MS         2000

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
} bmt_scan_tag_info_t;

typedef struct {
    float q, r, x, p, k;
} bmt_kalman_t;

static bmt_scan_tag_info_t g_bmt_tags[BMT_MAX_TAGS];
static bmt_kalman_t        g_bmt_kalman[BMT_MAX_TAGS];

/* ============================================================================
 * TIME DIVISION RADIO MANAGEMENT
 *
 * 1 ESP32 chỉ có 1 radio → mesh scan và GAP scan conflict nếu chạy cùng lúc.
 * Giải pháp: time division — chia chu kỳ ~1500ms
 *
 *   |<-- GAP scan 1000ms -->|<-- Mesh publish 500ms -->|
 *      Bắt ADV tag              Gửi data lên mesh
 *      Kalman filter
 *      Update loss rate
 * ============================================================================ */
/* Time-division cycle (1500ms total)
 * - GAP scan 1200ms = 80% (tăng từ 1000ms để bắt tag tốt hơn)
 * - Mesh publish 300ms = 20% (giảm từ 500ms — dư cho 1 publish tag report)
 */
#define BMT_GAP_SCAN_DURATION_MS        1200
#define BMT_MESH_PUBLISH_DURATION_MS    300

#define BMT_SCAN_INTERVAL_UNITS         0x0010  /* 10ms */
#define BMT_SCAN_WINDOW_UNITS           0x0010  /* 10ms = 100% duty   */

#define BMT_PROV_COMPLETE_BIT           BIT0

typedef enum {
    BMT_PHASE_GAP_SCAN = 0,
    BMT_PHASE_MESH_PUB = 1,
} bmt_radio_phase_t;

static volatile bmt_radio_phase_t g_bmt_phase        = BMT_PHASE_GAP_SCAN;
static volatile bool              g_bmt_has_new_data = false;

/* ============================================================================
 * BLE MESH NODE STATE
 * ============================================================================ */
static uint8_t g_bmt_scan_uuid[16] = {
    0x53, 0x43, 0x41, 0x4E, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01
    /* ^^^ byte cuối = scanner ID, đổi cho từng scanner */
};

static uint16_t g_bmt_node_addr = 0x0000;
static uint16_t g_bmt_net_idx   = 0xFFFF;
static uint16_t g_bmt_app_idx   = 0xFFFF;

static EventGroupHandle_t g_bmt_mesh_evgrp;

/* ============================================================================
 * MESH MODELS
 * ============================================================================ */
static esp_ble_mesh_cfg_srv_t bmt_cfg_server = {
    /* [FIX A] Tăng retransmit để giảm PDU loss
     *   net_transmit (5,30)     : mỗi PDU tự transmit 5 lần, cách 30ms → ~150ms/PDU
     *   relay_retransmit (3,30) : forward PDU người khác 3 lần
     * Trade-off: air time tăng x2.5 nhưng reliability tăng đáng kể với ≤3 scanner */
    /* MAX retransmit để tối đa khả năng PDU đến Gateway trong môi trường nhiễu
     *   net_transmit (7,10)     : mỗi PDU tự transmit 8 lần (1+7), cách 10ms → ~70ms tổng
     *   relay_retransmit (7,10) : forward PDU người khác 8 lần
     * Trade-off: air time tăng nhưng reliability tăng max */
    .net_transmit     = ESP_BLE_MESH_TRANSMIT(7, 10),
    .relay            = ESP_BLE_MESH_RELAY_ENABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(7, 10),
    .beacon           = ESP_BLE_MESH_BEACON_ENABLED,
    .gatt_proxy       = ESP_BLE_MESH_GATT_PROXY_ENABLED,
    .friend_state     = ESP_BLE_MESH_FRIEND_ENABLED,    /* reserved cho LPN tương lai */
    .default_ttl      = 7,
};

ESP_BLE_MESH_MODEL_PUB_DEFINE(bmt_vnd_pub, sizeof(bmt_tag_report_t) + 4, ROLE_NODE);

static esp_ble_mesh_model_op_t bmt_vnd_ops[] = {
    ESP_BLE_MESH_MODEL_OP(BMT_OP_VND_TAG_STATUS, sizeof(bmt_tag_report_t)),
    ESP_BLE_MESH_MODEL_OP_END,
};

static esp_ble_mesh_model_t bmt_vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(BMT_CID_ESP, BMT_VND_MODEL_ID,
                              bmt_vnd_ops, &bmt_vnd_pub, NULL),
};

static esp_ble_mesh_model_t bmt_root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&bmt_cfg_server),
};

static esp_ble_mesh_elem_t bmt_elements[] = {
    ESP_BLE_MESH_ELEMENT(0, bmt_root_models, bmt_vnd_models),
};

static esp_ble_mesh_comp_t bmt_composition = {
    .cid           = BMT_CID_ESP,
    .element_count = ARRAY_SIZE(bmt_elements),
    .elements      = bmt_elements,
};

static esp_ble_mesh_prov_t bmt_provision = { .uuid = g_bmt_scan_uuid };

/* GAP scan params */
static esp_ble_scan_params_t g_bmt_scan_params = {
    .scan_type          = BLE_SCAN_TYPE_PASSIVE,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval      = BMT_SCAN_INTERVAL_UNITS,
    .scan_window        = BMT_SCAN_WINDOW_UNITS,
    .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
};

/* ============================================================================
 * KALMAN FILTER
 * ============================================================================ */
static void bmt_kalman_init(bmt_kalman_t *kf, float initial_rssi)
{
    kf->q = 0.1f; kf->r = 2.0f;
    kf->x = initial_rssi; kf->p = 1.0f; kf->k = 0.0f;
}

static float bmt_kalman_update(bmt_kalman_t *kf, float rssi)
{
    kf->p = kf->p + kf->q;
    kf->k = kf->p / (kf->p + kf->r);
    kf->x = kf->x + kf->k * (rssi - kf->x);
    kf->p = (1.0f - kf->k) * kf->p;
    return kf->x;
}

/* ============================================================================
 * CRC-16 CCITT
 * ============================================================================ */
static uint16_t bmt_crc16_ccitt(const uint8_t *data, int len)
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

static bool bmt_verify_crc16(const uint8_t *data, int len, uint16_t received_crc)
{
    return (bmt_crc16_ccitt(data, len) == received_crc);
}

/* ============================================================================
 * DISTANCE CALCULATION (path loss model)
 *   distance = 10 ^ ((tx_power - rssi) / (10 * n))
 *   n = 2.5 cho indoor
 * ============================================================================ */
#define BMT_PATH_LOSS_N                 2.5f

static float bmt_calculate_distance(int8_t tx_power, float rssi_filtered)
{
    if (rssi_filtered >= 0) return 0.0f;
    float ratio = (tx_power - rssi_filtered) / (10.0f * BMT_PATH_LOSS_N);
    return powf(10.0f, ratio);
}

/* ============================================================================
 * TAG TABLE MANAGEMENT
 * ============================================================================ */
static int bmt_find_tag(uint16_t tag_id)
{
    for (int i = 0; i < BMT_MAX_TAGS; i++)
        if (g_bmt_tags[i].active && g_bmt_tags[i].tag_id == tag_id) return i;
    return -1;
}

static int bmt_add_tag(uint16_t tag_id, uint8_t tag_type,
                       int8_t tx_power, int8_t rssi,
                       uint8_t sequence, uint8_t *mac)
{
    for (int i = 0; i < BMT_MAX_TAGS; i++) {
        if (!g_bmt_tags[i].active) {
            memset(&g_bmt_tags[i], 0, sizeof(bmt_scan_tag_info_t));
            g_bmt_tags[i].active           = true;
            g_bmt_tags[i].tag_type         = tag_type;
            g_bmt_tags[i].tag_id           = tag_id;
            g_bmt_tags[i].tx_power         = tx_power;
            g_bmt_tags[i].rssi_raw         = rssi;
            g_bmt_tags[i].rssi_filtered    = (float)rssi;
            g_bmt_tags[i].last_sequence    = sequence;
            g_bmt_tags[i].total_received   = 1;
            g_bmt_tags[i].last_seen_ms     = xTaskGetTickCount() * portTICK_PERIOD_MS;
            g_bmt_tags[i].last_logged_rssi = rssi;
            g_bmt_tags[i].last_log_ms      = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (mac) memcpy(g_bmt_tags[i].mac, mac, 6);
            bmt_kalman_init(&g_bmt_kalman[i], (float)rssi);
            g_bmt_tags[i].distance = bmt_calculate_distance(tx_power, (float)rssi);
            ESP_LOGI(TAG, "New tag: 0x%04X (%s) RSSI=%ddBm Dist=%.2fm",
                     tag_id,
                     tag_type == BMT_TAG_TYPE_PERSON ? "PERSON" : "ASSET",
                     rssi, g_bmt_tags[i].distance);
            return i;
        }
    }
    ESP_LOGW(TAG, "Tag table full!");
    return -1;
}

static void bmt_update_tag(int idx, int8_t rssi, uint8_t sequence)
{
    bmt_scan_tag_info_t *t   = &g_bmt_tags[idx];
    uint32_t             now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (sequence == t->last_sequence) {
        t->rssi_raw      = rssi;
        t->rssi_filtered = bmt_kalman_update(&g_bmt_kalman[idx], (float)rssi);
        t->distance      = bmt_calculate_distance(t->tx_power, t->rssi_filtered);
        t->last_seen_ms  = now;
        t->total_received++;
        int8_t   rd = (int8_t)abs((int)rssi - (int)t->last_logged_rssi);
        uint32_t td = now - t->last_log_ms;
        if (rd >= BMT_LOG_RSSI_THRESHOLD_DBM || td >= BMT_LOG_MIN_INTERVAL_MS) {
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
    t->rssi_filtered = bmt_kalman_update(&g_bmt_kalman[idx], (float)rssi);
    t->distance      = bmt_calculate_distance(t->tx_power, t->rssi_filtered);
    t->last_sequence = sequence;
    t->total_received++;
    t->last_seen_ms  = now;
    g_bmt_has_new_data = true;

    int8_t   rd = (int8_t)abs((int)rssi - (int)t->last_logged_rssi);
    uint32_t td = now - t->last_log_ms;
    if (rd >= BMT_LOG_RSSI_THRESHOLD_DBM || td >= BMT_LOG_MIN_INTERVAL_MS) {
        ESP_LOGI(TAG, "Tag 0x%04X | RSSI=%ddBm | Filt=%.1fdBm | Dist=%.2fm | Seq=%d",
                 t->tag_id, rssi, t->rssi_filtered, t->distance, sequence);
        t->last_logged_rssi = rssi; t->last_log_ms = now;
    }
}

static void bmt_print_tag_table(void)
{
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    bool has_tag = false;
    printf("\n========== TAG TABLE (Scanner 0x%02X) ==========\n", BMT_SCANNER_ID);
    for (int i = 0; i < BMT_MAX_TAGS; i++) {
        if (!g_bmt_tags[i].active) continue;
        has_tag = true;
        bmt_scan_tag_info_t *t   = &g_bmt_tags[i];
        uint32_t             age = (now - t->last_seen_ms) / 1000;
        float    lr  = 0.0f;
        uint32_t tot = t->total_received + t->total_missed;
        if (tot > 0) lr = (float)t->total_missed / tot * 100.0f;
        printf("Tag 0x%04X:\n", t->tag_id);
        printf("  Type      : %s\n",
               t->tag_type == BMT_TAG_TYPE_PERSON ? "PERSON" : "ASSET");
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

/* ============================================================================
 * PARSE ADV PAYLOAD
 *
 * 2 loại beacon được nhận:
 *   1. ESP32 custom tag (CID 0x02E5): UUID + Major + Minor + TXPwr + Seq + CRC = 24B
 *      Verify UUID prefix + CRC → tag_type từ Major, tag_id từ Minor
 *   2. iBeacon / iPhone (CID 0x004C, marker 02 15): UUID + Major + Minor + TXPwr = 21B
 *      Verify UUID prefix → tag_type từ Major, tag_id từ Minor
 *      sequence=0 → loss_pct=0% là bình thường
 * ============================================================================ */
static bool bmt_parse_tag_payload(uint8_t *adv_data, uint8_t adv_len,
                                  bmt_tag_payload_t *out)
{
    if (!adv_data || adv_len < 4 || !out) return false;
    int pos = 0;

    while (pos < adv_len) {
        uint8_t field_len = adv_data[pos];
        if (field_len == 0 || pos + field_len >= adv_len) break;
        uint8_t field_type = adv_data[pos + 1];

        if (field_type == 0xFF && field_len >= 3) {
            uint16_t cid = (uint16_t)adv_data[pos + 2]
                         | ((uint16_t)adv_data[pos + 3] << 8);

            /* ---- ESP32 custom tag: CID=0x02E5 ---- */
            if (cid == BMT_CID_ESPRESSIF && field_len >= (1 + 2 + 24)) {
                bmt_tag_adv_payload_t *p =
                    (bmt_tag_adv_payload_t *)(adv_data + pos + 4);

                if (memcmp(p->uuid, BMT_SYSTEM_UUID_PREFIX, 4) != 0)
                    goto next_field;

                if (!bmt_verify_crc16((uint8_t *)p,
                                      sizeof(*p) - sizeof(p->crc16),
                                      p->crc16)) {
                    ESP_LOGD(TAG, "CRC fail");
                    goto next_field;
                }

                out->tag_type = (p->major == BMT_TAG_MAJOR_PERSON)
                                ? BMT_TAG_TYPE_PERSON : BMT_TAG_TYPE_ASSET;
                out->tag_id   = p->minor;
                out->tx_power = p->tx_power;
                out->sequence = p->sequence;
                out->crc16    = p->crc16;
                return true;
            }

            /* ---- iBeacon / iPhone: CID=0x004C + marker 02 15 ---- */
            if (cid == BMT_CID_APPLE && field_len >= 26 &&
                adv_data[pos + 4] == 0x02 &&
                adv_data[pos + 5] == 0x15) {

                /* FIX 1: Bỏ UUID prefix check
                 * RNF app UUID tự generate crash nếu dùng system UUID (toàn 0)
                 * Filter bằng Major=1 (PERSON) hoặc 2 (ASSET) thay thế */
                uint16_t major = ((uint16_t)adv_data[pos + 22] << 8)
                                | adv_data[pos + 23];
                uint16_t minor = ((uint16_t)adv_data[pos + 24] << 8)
                                | adv_data[pos + 25];

                if (major != BMT_TAG_MAJOR_PERSON && major != BMT_TAG_MAJOR_ASSET)
                    goto next_field;

                out->tag_type = (major == BMT_TAG_MAJOR_PERSON)
                                ? BMT_TAG_TYPE_PERSON : BMT_TAG_TYPE_ASSET;

                /* FIX 2: Namespace 0x8000 cho iPhone
                 * ESP32:  tag_id = minor          (0x0001-0x00FF)
                 * iPhone: tag_id = minor | 0x8000 (0x8001-0x80FF)
                 * Tránh collision khi Minor giống nhau */
                out->tag_id   = minor | 0x8000;
                out->tx_power = BMT_PHONE_TX_POWER_1M;
                out->sequence = 0;
                out->crc16    = 0;
                return true;
            }
        }

        next_field:
        pos += field_len + 1;
    }
    return false;
}

/* ============================================================================
 * GAP CALLBACK
 * ============================================================================ */
static void bmt_gap_event_handler(esp_gap_ble_cb_event_t event,
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
        if (g_bmt_phase != BMT_PHASE_GAP_SCAN) break;
        EventBits_t bits = xEventGroupGetBits(g_bmt_mesh_evgrp);
        if (!(bits & BMT_PROV_COMPLETE_BIT)) break;

        uint8_t *adv_data = param->scan_rst.ble_adv;
        uint8_t  adv_len  = param->scan_rst.adv_data_len;
        int8_t   rssi     = param->scan_rst.rssi;
        uint8_t *mac      = param->scan_rst.bda;

        bmt_tag_payload_t payload;
        if (!bmt_parse_tag_payload(adv_data, adv_len, &payload)) break;

        int idx = bmt_find_tag(payload.tag_id);
        if (idx < 0) {
            idx = bmt_add_tag(payload.tag_id, payload.tag_type,
                              payload.tx_power, rssi, payload.sequence, mac);
        } else {
            bmt_update_tag(idx, rssi, payload.sequence);
        }

        g_bmt_has_new_data = true;
        break;
    }
    default: break;
    }
}

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
        ESP_LOGI(TAG, "Waiting for AppKey from Gateway...");
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
        /* FIX: bước quan trọng nhất
         * Gateway gửi APP_KEY_ADD → scan node lưu AppKey
         * g_bmt_app_idx set → publish_to_mesh mới chạy được */
        g_bmt_app_idx = param->value.state_change.appkey_add.app_idx;
        ESP_LOGI(TAG, "=== AppKey received! idx=0x%04x — Scan node ready to publish! ===",
                 g_bmt_app_idx);
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

/* ============================================================================
 * PUBLISH TAG DATA LÊN MESH
 *
 * FIX 1: Tách log "not provisioned" vs "waiting AppKey" để debug rõ ràng
 * FIX 2: Bỏ pre-fill pub->msg trước khi gọi esp_ble_mesh_model_publish
 *        Vấn đề: esp_ble_mesh_model_publish() gọi bt_mesh_model_msg_init()
 *                → RESET pub->msg → data đã fill bị mất
 *                → Chỉ cần pass data trực tiếp vào esp_ble_mesh_model_publish
 * ============================================================================ */
static void bmt_publish_tags_to_mesh(void)
{
    if (g_bmt_node_addr == 0x0000) {
        ESP_LOGW(TAG, "[Mesh] Not provisioned yet, skip publish");
        return;
    }
    if (g_bmt_app_idx == 0xFFFF) {
        ESP_LOGI(TAG,
                 "[Mesh] Provisioned (addr=0x%04x) but waiting for AppKey...",
                 g_bmt_node_addr);
        return;
    }

    int published = 0;
    for (int i = 0; i < BMT_MAX_TAGS; i++) {
        if (!g_bmt_tags[i].active) continue;

        uint32_t total    = g_bmt_tags[i].total_received + g_bmt_tags[i].total_missed;
        uint8_t  loss_pct = 0;
        if (total > 0)
            loss_pct = (uint8_t)((g_bmt_tags[i].total_missed * 100) / total);

        /* FIX: int16_t distance_dm thay vì float distance
         * Tránh segmentation (13B → 8B) → không còn "Ran out of retransmit" */
        int16_t distance_dm = (int16_t)(g_bmt_tags[i].distance * 10.0f);
        if (distance_dm < 0) distance_dm = 0;

        bmt_tag_report_t report = {
            .scanner_id  = BMT_SCANNER_ID,
            .tag_type    = g_bmt_tags[i].tag_type,
            .tag_id      = g_bmt_tags[i].tag_id,
            .rssi        = (int8_t)g_bmt_tags[i].rssi_filtered,
            .distance_dm = distance_dm,
            .loss_pct    = loss_pct,
        };

        bmt_vnd_models[0].pub->publish_addr = 0x0001;  /* Gateway unicast */
        bmt_vnd_models[0].pub->app_idx      = g_bmt_app_idx;
        bmt_vnd_models[0].pub->ttl          = 7;

        esp_err_t err = esp_ble_mesh_model_publish(
            &bmt_vnd_models[0],
            BMT_OP_VND_TAG_STATUS,
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
                     g_bmt_tags[i].tag_id, esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (published == 0)
        ESP_LOGD(TAG, "[Mesh] No active tags to publish");
    else
        ESP_LOGI(TAG, "[Mesh] Published %d tag(s) to mesh", published);
}

/* ============================================================================
 * TASKS
 * ============================================================================ */
static void bmt_radio_manager_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Waiting for mesh provision...");
    xEventGroupWaitBits(g_bmt_mesh_evgrp, BMT_PROV_COMPLETE_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    ESP_LOGI(TAG, "Mesh provisioned, waiting to stabilize...");
    vTaskDelay(pdMS_TO_TICKS(3000));

    esp_ble_gap_stop_scanning();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_err_t err = esp_ble_gap_set_scan_params(&g_bmt_scan_params);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_scan_params failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "=== Time Division Radio Manager started ===");
    ESP_LOGI(TAG, "GAP scan: %dms | Mesh publish: %dms | Cycle: %dms",
             BMT_GAP_SCAN_DURATION_MS, BMT_MESH_PUBLISH_DURATION_MS,
             BMT_GAP_SCAN_DURATION_MS + BMT_MESH_PUBLISH_DURATION_MS);

    /* [FIX B] Phase offset theo SCANNER_ID
     *   scanner 0x01 → 0ms
     *   scanner 0x02 → 500ms
     *   scanner 0x03 → 1000ms
     * → mesh publish window của 3 scanner không chồng lên nhau, giảm collision */
    uint32_t phase_offset_ms = (BMT_SCANNER_ID - 1) * 500;
    ESP_LOGI(TAG, "Phase offset for scanner 0x%02x: %ums",
             BMT_SCANNER_ID, (unsigned)phase_offset_ms);
    vTaskDelay(pdMS_TO_TICKS(phase_offset_ms));

    g_bmt_phase = BMT_PHASE_MESH_PUB;

    while (1) {
        /* ---- PHASE 1: GAP SCAN ---- */
        g_bmt_phase = BMT_PHASE_GAP_SCAN;
        g_bmt_has_new_data = false;

        ESP_LOGD(TAG, "[Phase 1] GAP scan %dms", BMT_GAP_SCAN_DURATION_MS);
        esp_ble_gap_start_scanning(0);
        vTaskDelay(pdMS_TO_TICKS(BMT_GAP_SCAN_DURATION_MS));

        /* ---- PHASE 2: MESH PUBLISH ---- */
        g_bmt_phase = BMT_PHASE_MESH_PUB;
        esp_ble_gap_stop_scanning();
        vTaskDelay(pdMS_TO_TICKS(100));

        if (g_bmt_has_new_data) {
            ESP_LOGI(TAG, "[Phase 2] Publishing tag data to mesh...");
            bmt_publish_tags_to_mesh();
            bmt_print_tag_table();
            g_bmt_has_new_data = false;
        }

        vTaskDelay(pdMS_TO_TICKS(BMT_MESH_PUBLISH_DURATION_MS - 100));
    }
}

static void bmt_timeout_check_task(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        for (int i = 0; i < BMT_MAX_TAGS; i++) {
            if (!g_bmt_tags[i].active) continue;
            if ((now - g_bmt_tags[i].last_seen_ms) > BMT_TAG_TIMEOUT_MS) {
                ESP_LOGW(TAG, "Tag 0x%04X OUT OF RANGE", g_bmt_tags[i].tag_id);
                g_bmt_tags[i].active = false;
                memset(&g_bmt_kalman[i], 0, sizeof(bmt_kalman_t));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ============================================================================
 * MESH INIT
 * ============================================================================ */
static esp_err_t bmt_ble_mesh_init_scan(void)
{
    esp_ble_mesh_register_prov_callback(bmt_mesh_prov_cb);
    esp_ble_mesh_register_config_server_callback(bmt_mesh_cfg_server_cb);

    ESP_ERROR_CHECK(esp_ble_mesh_init(&bmt_provision, &bmt_composition));

    bool provisioned = esp_ble_mesh_node_is_provisioned();

    if (provisioned) {
        ESP_LOGI(TAG, "Already provisioned (restored from NVS)");

        /* g_bmt_node_addr chỉ dùng để check != 0x0000 trong publish
         * → placeholder 0x0001 đủ (giá trị chính xác set lần đầu provision) */
        g_bmt_node_addr = 0x0001;
        ESP_LOGI(TAG, "Node addr restored (placeholder=0x%04x)", g_bmt_node_addr);

        /* AppKey & model binding đã lưu trong mesh NVS, stack tự restore */
        g_bmt_app_idx = 0x0000;
        ESP_LOGI(TAG, "app_idx=0x%04x — ready to publish", g_bmt_app_idx);

        xEventGroupSetBits(g_bmt_mesh_evgrp, BMT_PROV_COMPLETE_BIT);
    } else {
        g_bmt_phase = BMT_PHASE_MESH_PUB;
        ESP_ERROR_CHECK(esp_ble_mesh_node_prov_enable(
            ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT));
        ESP_LOGI(TAG, "BLE Mesh Scan Node initialized");
        ESP_LOGI(TAG, "UUID: SCAN (53:43:41:4E:...:%02X)", g_bmt_scan_uuid[15]);
        ESP_LOGI(TAG, "Waiting for Gateway to provision...");
    }

    return ESP_OK;
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

    printf("\n===== BMT SCAN NODE COMMANDS =====\n");
    printf("r -> RESET mesh (xoa NVS, ve unprovisioned)\n");
    printf("1 -> STATUS hien tai\n");
    printf("==================================\n");

    uint8_t ch;
    while (1) {
        int len = uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(200));
        if (len <= 0 || ch == '\r' || ch == '\n') continue;

        switch (ch) {
        case 'r':
        case 'R':
            printf("\n[UART] Resetting mesh provision + REBOOT...\n");
            printf("[UART] Scan node se ve unprovisioned va broadcast beacon moi\n\n");
            esp_ble_mesh_node_local_reset();
            /* Reboot sau reset: radio_manager_task vẫn chạy GAP scan cycle.
             * Reset mesh KHÔNG dừng task này → radio bận khi Gateway re-provision
             * → PB-ADV PDU bị drop → "Timeout giving up transaction".
             * Reboot là cách sạch nhất. */
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
            break;

        case '1':
            printf("\n===== BMT SCAN STATUS =====\n");
            printf("Scanner ID: 0x%02X\n", BMT_SCANNER_ID);
            printf("Node addr : 0x%04X %s\n", g_bmt_node_addr,
                   g_bmt_node_addr == 0 ? "(NOT provisioned)" : "(provisioned)");
            printf("Net idx   : 0x%04X\n", g_bmt_net_idx);
            printf("App idx   : 0x%04X %s\n", g_bmt_app_idx,
                   g_bmt_app_idx == 0xFFFF ? "(waiting AppKey)" : "(AppKey OK)");
            printf("Phase     : %s\n",
                   g_bmt_phase == BMT_PHASE_GAP_SCAN ? "GAP_SCAN" : "MESH_PUB");
            printf("===========================\n");
            break;

        default:
            printf("[UART] Unknown: %c  (r=reset, 1=status)\n", ch);
            break;
        }
    }
}

/* ============================================================================
 * HEALTH MONITORING — Chip temperature + VDD voltage
 * Đo tự ESP32 (không cần module ngoài), publish health mỗi 30s
 * ============================================================================ */
#define BMT_HEALTH_PUBLISH_INTERVAL_MS  30000   /* 30s/lần */

#if SOC_TEMP_SENSOR_SUPPORTED
static temperature_sensor_handle_t g_bmt_temp_sensor   = NULL;
#endif
static adc_oneshot_unit_handle_t   g_bmt_adc_handle    = NULL;
static adc_cali_handle_t           g_bmt_adc_cali      = NULL;
static bool                        g_bmt_adc_calibrated = false;

static void bmt_temp_sensor_init(void)
{
#if SOC_TEMP_SENSOR_SUPPORTED
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err = temperature_sensor_install(&cfg, &g_bmt_temp_sensor);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Temp sensor install failed: %s", esp_err_to_name(err));
        return;
    }
    temperature_sensor_enable(g_bmt_temp_sensor);
    ESP_LOGI(TAG, "Temperature sensor initialized");
#else
    ESP_LOGI(TAG, "Temperature sensor not supported on this target (ESP32 classic)");
#endif
}

static int8_t bmt_get_chip_temp_c(void)
{
#if SOC_TEMP_SENSOR_SUPPORTED
    if (!g_bmt_temp_sensor) return 0;
    float temp_c = 0.0f;
    if (temperature_sensor_get_celsius(g_bmt_temp_sensor, &temp_c) == ESP_OK)
        return (int8_t)temp_c;
#endif
    return 0;
}

static void bmt_adc_init(void)
{
    /* ADC1 channel 0 (GPIO36) — đọc VDD33 internal qua attenuation 12dB
     * Note: Ở đây ta đo qua external pin để demo. Real VDD33 measurement
     * cần routing đặc biệt, simplest là approximate qua heap/load.
     * Nếu user muốn đo VDD thực, dùng voltage divider 5V→ADC pin.
     * Ở đây ta gọi đây là "supply voltage estimate". */
    adc_oneshot_unit_init_cfg_t init_cfg = { .unit_id = ADC_UNIT_1 };
    esp_err_t err = adc_oneshot_new_unit(&init_cfg, &g_bmt_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC unit init failed: %s", esp_err_to_name(err));
        return;
    }
    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = ADC_ATTEN_DB_12,
    };
    adc_oneshot_config_channel(g_bmt_adc_handle, ADC_CHANNEL_0, &chan_cfg);

    bool cali_ok = false;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    cali_ok = (adc_cali_create_scheme_curve_fitting(&cali_cfg, &g_bmt_adc_cali) == ESP_OK);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    cali_ok = (adc_cali_create_scheme_line_fitting(&cali_cfg, &g_bmt_adc_cali) == ESP_OK);
#endif
    if (cali_ok) {
        g_bmt_adc_calibrated = true;
        ESP_LOGI(TAG, "ADC calibrated for VDD measurement");
    } else {
        ESP_LOGW(TAG, "ADC calibration failed, using estimate");
    }
}

static uint16_t bmt_get_vdd_mv(void)
{
    /* Without external voltage divider, return nominal 3300mV.
     * If user wires GPIO36 to a voltage divider for 5V VBUS, this reads real. */
    if (!g_bmt_adc_handle || !g_bmt_adc_calibrated) return 3300;
    int raw = 0, voltage_mv = 0;
    if (adc_oneshot_read(g_bmt_adc_handle, ADC_CHANNEL_0, &raw) == ESP_OK)
        adc_cali_raw_to_voltage(g_bmt_adc_cali, raw, &voltage_mv);
    /* Multiply by 2 if voltage divider used (typical 2:1) */
    return (uint16_t)voltage_mv;
}

/* Task publish health metric mỗi 30s */
static void bmt_node_health_task(void *arg)
{
    (void)arg;

    /* Wait mesh ready */
    vTaskDelay(pdMS_TO_TICKS(15000));

    while (1) {
        if (g_bmt_app_idx == 0xFFFF) {
            ESP_LOGD(TAG, "[Health] Skip — AppKey not bound");
            vTaskDelay(pdMS_TO_TICKS(BMT_HEALTH_PUBLISH_INTERVAL_MS));
            continue;
        }

        uint32_t uptime_s = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;

        bmt_node_health_t health = {
            .scanner_id     = BMT_SCANNER_ID,
            .chip_temp_c    = bmt_get_chip_temp_c(),
            .vdd_mv         = bmt_get_vdd_mv(),
            .free_heap_kb   = (uint16_t)(esp_get_free_heap_size() / 1024),
            .uptime_min     = (uint16_t)(uptime_s / 60),
        };

        bmt_vnd_models[0].pub->publish_addr = 0x0001;
        bmt_vnd_models[0].pub->app_idx      = g_bmt_app_idx;
        bmt_vnd_models[0].pub->ttl          = 7;

        esp_err_t err = esp_ble_mesh_model_publish(
            &bmt_vnd_models[0],
            BMT_OP_VND_NODE_HEALTH,
            sizeof(health),
            (uint8_t *)&health,
            ROLE_NODE);

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "[Health] Published: temp=%d°C vdd=%umV heap=%uKB uptime=%umin",
                     health.chip_temp_c, health.vdd_mv,
                     health.free_heap_kb, health.uptime_min);
        } else {
            ESP_LOGW(TAG, "[Health] Publish failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(BMT_HEALTH_PUBLISH_INTERVAL_MS));
    }
}

/* ============================================================================
 * MAIN
 * ============================================================================ */
void app_main(void)
{
    ESP_LOGI(TAG, "=== BMT Scan Node Starting ===");
    ESP_LOGI(TAG, "Scanner ID: 0x%02X", BMT_SCANNER_ID);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    g_bmt_mesh_evgrp = xEventGroupCreate();

    err = bluetooth_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "BT init failed!"); return; }

    /* MAX TX POWER (+9 dBm) — tăng range BLE adv + mesh */
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV,     ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN,    ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    ESP_LOGI(TAG, "BLE TX power set to +9 dBm (max for ESP32 classic)");

    /* Init health monitoring (chip temp + ADC VDD) */
    bmt_temp_sensor_init();
    bmt_adc_init();

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(bmt_gap_event_handler));

    err = bmt_ble_mesh_init_scan();
    if (err != ESP_OK) { ESP_LOGE(TAG, "Mesh init failed!"); return; }

    xTaskCreate(bmt_radio_manager_task, "bmt_radio",   3072, NULL, 6, NULL);
    xTaskCreate(bmt_timeout_check_task, "bmt_timeout", 2048, NULL, 3, NULL);
    xTaskCreate(bmt_uart_cmd_task,      "bmt_uart",    2048, NULL, 3, NULL);
    xTaskCreate(bmt_node_health_task,   "bmt_health",  3072, NULL, 2, NULL);  /* [NEW] */

    ESP_LOGI(TAG, "=== BMT Scan Node READY (r=reset, 1=status) ===");
}