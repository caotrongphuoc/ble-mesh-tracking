#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "BLE_TAG"

/* ========================================================================== 
 * TỰ BUILD — TAG CONFIG
 * ========================================================================== */
#define TAG_MAGIC               0xAB
#define TAG_TYPE_PERSON         0x01
#define TAG_TYPE_ASSET          0x02

#define TAG_PERSON_001          0x0001
#define TAG_PERSON_002          0x0002
#define TAG_PERSON_003          0x0003
#define TAG_ASSET_001           0x0100
#define TAG_ASSET_002           0x0101

/* TX Power tại 1m — placeholder -59dBm (chuẩn iBeacon)
 * Phải đo thực tế: đặt tag cách scanner 1m
 * đo RSSI 100 lần, lấy trung bình → ghi vào đây */
#define TAG_TX_POWER            -59

/* ADV Interval range (ms)
 * Random trong khoảng 450-550ms mỗi lần phát
 * Tránh collision khi nhiều tag cùng phát */
#define ADV_INTERVAL_MIN_MS     450
#define ADV_INTERVAL_MAX_MS     550
#define ADV_INTERVAL_UNITS      800   /* 500ms = (450+550)/2 */

/* ========================================================================== 
 * TỰ BUILD — PAYLOAD STRUCT (8 bytes)
 * ========================================================================== */
#pragma pack(1)
typedef struct {
    uint8_t  magic;     /* 0xAB — chữ ký hệ thống */
    uint8_t  tag_type;  /* 0x01=PERSON, 0x02=ASSET */
    uint16_t tag_id;    /* ID unique (big endian) */
    int8_t   tx_power;  /* công suất phát tại 1m */
    uint8_t  sequence;  /* tăng mỗi lần phát */
    uint16_t crc16;     /* CRC-16 CCITT từ 6 bytes đầu */
} tag_payload_t;        /* tổng 8 bytes */
#pragma pack()

/* ========================================================================== 
 * GLOBALS
 * ========================================================================== */
static tag_payload_t g_tag;
static uint8_t       g_adv_raw[31];
static uint8_t       g_adv_len  = 0;
static uint8_t       g_mac[6]   = {0};  /* MAC address của ESP32 này */

/* ========================================================================== 
 * DÙNG API — ADV PARAMS
 * ========================================================================== */
static esp_ble_adv_params_t g_adv_params = {
    .adv_int_min       = ADV_INTERVAL_UNITS,
    .adv_int_max       = ADV_INTERVAL_UNITS,
    .adv_type          = ADV_TYPE_NONCONN_IND,
    .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
    .channel_map       = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

/* ========================================================================== 
 * TỰ BUILD — CRC-16 CCITT
 * ========================================================================== */
static uint16_t crc16_ccitt(uint8_t *data, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc <<= 1;
        }
    }
    return crc;
}

/* ========================================================================== 
 * TỰ BUILD — BUILD ADV PACKET
 * Format: [Flags: 3B][Manufacturer Specific: 12B]
 * Tổng: 15 bytes (< 31 bytes BLE spec limit)
 * ========================================================================== */
static void build_adv_data(void)
{
    uint8_t idx = 0;

    /* AD1: Flags */
    g_adv_raw[idx++] = 0x02;
    g_adv_raw[idx++] = 0x01;
    g_adv_raw[idx++] = 0x06;

    /* Build 8-byte payload */
    uint8_t payload[8];
    payload[0] = g_tag.magic;
    payload[1] = g_tag.tag_type;
    payload[2] = (g_tag.tag_id >> 8) & 0xFF;
    payload[3] = g_tag.tag_id & 0xFF;
    payload[4] = (uint8_t)g_tag.tx_power;
    payload[5] = g_tag.sequence;

    uint16_t crc = crc16_ccitt(payload, 6);
    payload[6]   = (crc >> 8) & 0xFF;
    payload[7]   = crc & 0xFF;

    /* AD2: Manufacturer Specific */
    g_adv_raw[idx++] = 0x0B;
    g_adv_raw[idx++] = 0xFF;
    g_adv_raw[idx++] = 0xE5;
    g_adv_raw[idx++] = 0x02;
    memcpy(&g_adv_raw[idx], payload, sizeof(payload));
    idx += sizeof(payload);

    g_adv_len = idx;
}

/* ========================================================================== 
 * PRINT HELPERS
 * ========================================================================== */
static void print_tag_info(const char *title)
{
    printf("\n============ %s ============\n", title);
    printf("Magic    : 0x%02X\n",   g_tag.magic);
    printf("Type     : 0x%02X (%s)\n",
           g_tag.tag_type,
           g_tag.tag_type == TAG_TYPE_PERSON ? "PERSON" : "ASSET");
    printf("ID       : 0x%04X\n",   g_tag.tag_id);
    printf("TX Power : %d dBm\n",   g_tag.tx_power);
    printf("Sequence : %d\n",       g_tag.sequence);
    printf("ADV Len  : %d bytes\n", g_adv_len);
    printf("Interval : %d-%d ms (random)\n",
           ADV_INTERVAL_MIN_MS, ADV_INTERVAL_MAX_MS);
    /* FIX: In MAC address */
    printf("MAC      : %02X:%02X:%02X:%02X:%02X:%02X\n",
           g_mac[0], g_mac[1], g_mac[2],
           g_mac[3], g_mac[4], g_mac[5]);
    printf("==============================\n");
}

/* ========================================================================== 
 * DÙNG API — GAP CALLBACK
 * ========================================================================== */
static void gap_event_handler(esp_gap_ble_cb_event_t event,
                               esp_ble_gap_cb_param_t *param)
{
    switch (event) {

    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        esp_ble_gap_start_advertising(&g_adv_params);
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Advertising started!");
            print_tag_info("TAG ADVERTISING");
        } else {
            ESP_LOGE(TAG, "Advertising FAILED, status=%d",
                     param->adv_start_cmpl.status);
        }
        break;

    default:
        break;
    }
}

/* ========================================================================== 
 * TỰ BUILD — SEQUENCE UPDATE TASK
 * ========================================================================== */
static void seq_update_task(void *arg)
{
    (void)arg;

    uint32_t start_delay = ADV_INTERVAL_MIN_MS +
                           (esp_random() % (ADV_INTERVAL_MAX_MS -
                                            ADV_INTERVAL_MIN_MS));
    vTaskDelay(pdMS_TO_TICKS(start_delay));

    ESP_LOGI(TAG, "Sequence update task started (random %d-%dms)",
             ADV_INTERVAL_MIN_MS, ADV_INTERVAL_MAX_MS);

    while (1) {
        g_tag.sequence++;
        build_adv_data();
        esp_ble_gap_config_adv_data_raw(g_adv_raw, g_adv_len);
        ESP_LOGD(TAG, "Seq=%d", g_tag.sequence);

        uint32_t interval = ADV_INTERVAL_MIN_MS +
                            (esp_random() % (ADV_INTERVAL_MAX_MS -
                                             ADV_INTERVAL_MIN_MS));
        vTaskDelay(pdMS_TO_TICKS(interval));
    }
}

/* ========================================================================== 
 * TỰ BUILD — TAG INIT
 * ========================================================================== */
static void tag_init(uint8_t type, uint16_t id)
{
    g_tag.magic    = TAG_MAGIC;
    g_tag.tag_type = type;
    g_tag.tag_id   = id;
    g_tag.tx_power = TAG_TX_POWER;
    g_tag.sequence = 0;

    ESP_LOGI(TAG, "Tag init: type=0x%02X id=0x%04X txpwr=%d",
             type, id, TAG_TX_POWER);
}

/* ========================================================================== 
 * DÙNG API — BLE INIT
 * FIX: Thêm đọc MAC address sau khi bluedroid enable
 * ========================================================================== */
static esp_err_t ble_init(void)
{
    esp_err_t err;

    err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mem_release failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) return err;

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) return err;

    err = esp_bluedroid_init();
    if (err != ESP_OK) return err;

    err = esp_bluedroid_enable();
    if (err != ESP_OK) return err;

    /* FIX: Đọc MAC address của ESP32 này
     * MAC = địa chỉ vật lý phần cứng
     * Dùng để identify tag cụ thể
     * Hiển thị trong print_tag_info() */
    esp_read_mac(g_mac, ESP_MAC_BT);
    ESP_LOGI(TAG, "BLE MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             g_mac[0], g_mac[1], g_mac[2],
             g_mac[3], g_mac[4], g_mac[5]);

    err = esp_ble_gap_register_callback(gap_event_handler);
    if (err != ESP_OK) return err;

    err = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_N0);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "BLE initialized, TX power=0dBm");
    return ESP_OK;
}

/* ========================================================================== 
 * MAIN
 * ========================================================================== */
void app_main(void)
{
    esp_err_t err;

    ESP_LOGI(TAG, "=== BLE Tag Starting ===");

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = ble_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed!");
        return;
    }

    /* =====================================================
     * CONFIG TAG — ĐỔI DÒNG NÀY KHI FLASH TỪNG TAG
     * =====================================================
     * Person 001: tag_init(TAG_TYPE_PERSON, TAG_PERSON_001)
     * Person 002: tag_init(TAG_TYPE_PERSON, TAG_PERSON_002)
     * Asset  001: tag_init(TAG_TYPE_ASSET,  TAG_ASSET_001)
     * Asset  002: tag_init(TAG_TYPE_ASSET,  TAG_ASSET_002)
     * ===================================================== */
    tag_init(TAG_TYPE_PERSON, TAG_PERSON_001);

    build_adv_data();

    err = esp_ble_gap_config_adv_data_raw(g_adv_raw, g_adv_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config_adv_data_raw failed!");
        return;
    }

    xTaskCreate(seq_update_task, "seq_update", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "=== TAG READY ===");
}