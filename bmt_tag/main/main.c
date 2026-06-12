#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_random.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "driver/uart.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"

/* ==========================================================================
 * TAG — BLE BEACON
 *
 * Payload format (24 bytes) so sánh với iBeacon:
 *
 *   iBeacon (Apple CID 0x004C):
 *     CID(2) + 0215(2) + UUID(16) + Major(2) + Minor(2) + TXPwr(1) = 25B
 *
 *   Custom (Espressif CID 0x02E5):
 *     CID(2) + UUID(16) + Major(2) + Minor(2) + TXPwr(1) + Seq(1) + CRC(2) = 26B
 *
 *   Bỏ "02 15" marker (chỉ cần cho iOS CLBeaconRegion, không cần với scanner)
 *   → tiết kiệm 2B → có chỗ cho Seq + CRC, tổng ADV = 31B (vừa khít)
 *
 * iPhone dùng RNF Beacon Toolkit:
 *   Set UUID = AB000000-..., Major = 1 (PERSON), Minor = tag_id
 *   Scanner detect qua CID 0x004C → không có Seq/CRC → loss = 0%
 * ========================================================================== */

#define TAG "BLE_TAG"

/* ==========================================================================
 * USER CONFIG — ĐỔI CHO TỪNG TAG
 * ========================================================================== */

/* System UUID — GIỐNG NHAU trên tất cả tag (ESP32 + iPhone)
 * Scanner check 4 bytes đầu (AB 00 00 00) để nhận ra "tag của hệ thống" */
static const uint8_t SYSTEM_UUID[16] = {
    0xAB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* Major: loại tag
 * 0x0001 = PERSON (người đeo)
 * 0x0002 = ASSET  (tài sản gắn thiết bị) */
#define TAG_MAJOR        0x0001   /* ← đổi cho từng loại */

/* Minor: ID thiết bị trong hệ thống
 * Tag ESP32 #1 = 0x0001, #2 = 0x0002...
 * iPhone RNF: set cùng Minor tương ứng */
#define TAG_MINOR        0x0001   /* ← đổi cho từng thiết bị */

/* TX Power reference: RSSI đo được tại đúng 1m (giá trị calibrate)
 * KHÔNG phải radio TX power thực — đây là "Measured Power" như iBeacon
 * Mặc định -59 (iBeacon standard), cần đo thực tế để chỉnh lại */
#define TAG_TX_POWER     (-53)    /* ← calibrate: đo RSSI tại 1m rồi điền */

/* Radio TX power thực (ảnh hưởng range + pin)
 * ESP_PWR_LVL_N0 = 0 dBm → range ~10-15m indoor, phù hợp cho hệ thống này */
#define TAG_RADIO_PWR    ESP_PWR_LVL_N0

/* ADV interval random trong range này để tránh collision nhiều tag */
#define ADV_INTERVAL_MIN_MS  450
#define ADV_INTERVAL_MAX_MS  550

/* ==========================================================================
 * PAYLOAD STRUCT
 * ========================================================================== */
#pragma pack(1)
typedef struct {
    uint8_t  uuid[16];   /* 16B: system UUID (AB000000-...) */
    uint16_t major;      /*  2B: PERSON=0x0001, ASSET=0x0002 */
    uint16_t minor;      /*  2B: tag ID trong hệ thống */
    int8_t   tx_power;   /*  1B: measured power tại 1m (calibrate) */
    uint8_t  sequence;   /*  1B: 0–255, wraps → Scanner tính loss rate */
    uint16_t crc16;      /*  2B: CRC-16 CCITT của 22 bytes trước */
} tag_adv_payload_t;     /* = 24 bytes */
#pragma pack()

/* ==========================================================================
 * GLOBALS
 * ========================================================================== */
static uint8_t       g_sequence   = 0;
static TimerHandle_t g_seq_timer  = NULL;
static bool          g_adv_active = false;

/* Raw ADV buffer — 31 bytes (maximum BLE ADV payload)
 *
 * Layout:
 *   [0..2]   Flags:   02 01 06
 *   [3]      Mfr len: 0x1B = 27 (= 1 type + 2 CID + 24 payload)
 *   [4]      Type:    0xFF (Manufacturer Specific)
 *   [5..6]   CID:     E5 02 (Espressif, little-endian)
 *   [7..30]  Payload: tag_adv_payload_t (24 bytes)
 */
#define ADV_RAW_LEN     31
#define ADV_PAYLOAD_OFF  7

static uint8_t g_adv_raw[ADV_RAW_LEN] = {
    /* Flags */
    0x02, 0x01, 0x06,
    /* Manufacturer Specific Data header */
    0x1B, 0xFF, 0xE5, 0x02,
    /* 24 bytes payload (filled by build_adv_data) */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* ==========================================================================
 * CRC-16 CCITT
 * ========================================================================== */
static uint16_t crc16_ccitt(const uint8_t *data, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    }
    return crc;
}

/* ==========================================================================
 * BUILD ADV DATA
 * ========================================================================== */
static void build_adv_data(void)
{
    tag_adv_payload_t p;

    memcpy(p.uuid,  SYSTEM_UUID, 16);
    p.major    = TAG_MAJOR;
    p.minor    = TAG_MINOR;
    p.tx_power = TAG_TX_POWER;
    p.sequence = g_sequence;
    p.crc16    = 0;   /* bắt buộc = 0 trước khi tính CRC */

    /* CRC-16 tính trên tất cả fields trừ 2 bytes crc16 cuối */
    p.crc16 = crc16_ccitt((uint8_t *)&p,
                           sizeof(p) - sizeof(p.crc16));

    memcpy(g_adv_raw + ADV_PAYLOAD_OFF, &p, sizeof(p));
}

/* ==========================================================================
 * ADV PARAMS & START
 * ========================================================================== */
static esp_ble_adv_params_t g_adv_params = {
    .adv_int_min       = 0,
    .adv_int_max       = 0,
    .adv_type          = ADV_TYPE_NONCONN_IND,
    .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
    .peer_addr_type    = BLE_ADDR_TYPE_PUBLIC,
    .channel_map       = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void start_adv_random_interval(void)
{
    /* Random [450, 550] ms → tránh collision nhiều tag */
    uint32_t ms = ADV_INTERVAL_MIN_MS
                + (esp_random() % (ADV_INTERVAL_MAX_MS
                                   - ADV_INTERVAL_MIN_MS + 1));

    /* BLE unit = 0.625ms */
    uint16_t units = (uint16_t)((ms * 1000) / 625);
    g_adv_params.adv_int_min = units;
    g_adv_params.adv_int_max = units;

    build_adv_data();

    /* config_adv_data_raw → GAP callback → start_advertising */
    esp_ble_gap_config_adv_data_raw(g_adv_raw, ADV_RAW_LEN);
}

/* ==========================================================================
 * SEQUENCE TIMER
 *
 * Non-connectable ADV (ADV_NONCONN_IND) không trigger ADV_STOP_COMPLETE_EVT
 * → không thể increment sequence trong callback
 * → dùng FreeRTOS timer ~500ms để increment + restart ADV
 * ========================================================================== */
static void seq_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    g_sequence++;   /* uint8_t: 255 → 0 tự động */

    esp_ble_gap_stop_advertising();
    start_adv_random_interval();
}

/* ==========================================================================
 * GAP CALLBACK
 * ========================================================================== */
static void gap_event_handler(esp_gap_ble_cb_event_t event,
                               esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        if (param->adv_data_raw_cmpl.status == ESP_BT_STATUS_SUCCESS)
            esp_ble_gap_start_advertising(&g_adv_params);
        else
            ESP_LOGE(TAG, "ADV data set FAILED");
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            g_adv_active = true;
            ESP_LOGD(TAG, "ADV OK seq=%u major=0x%04X minor=0x%04X",
                     g_sequence, TAG_MAJOR, TAG_MINOR);
        } else {
            ESP_LOGE(TAG, "ADV start FAILED");
        }
        break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        g_adv_active = false;
        break;

    default:
        break;
    }
}

/* ==========================================================================
 * BT INIT
 * ========================================================================== */
static esp_err_t bluetooth_init(void)
{
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    esp_bluedroid_config_t cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    /* Set radio TX power — ảnh hưởng range và pin */
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, TAG_RADIO_PWR);

    return ESP_OK;
}

/* ==========================================================================
 * UART STATUS TASK
 *   1 → in trạng thái hiện tại
 * ========================================================================== */
static void uart_task(void *arg)
{
    (void)arg;
    const uart_config_t cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_0, &cfg);

    printf("\n===== TAG COMMANDS: 1=status =====\n");

    uint8_t ch;
    while (1) {
        int len = uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(200));
        if (len <= 0 || ch == '\r' || ch == '\n') continue;

        if (ch == '1') {
            const tag_adv_payload_t *p =
                (const tag_adv_payload_t *)(g_adv_raw + ADV_PAYLOAD_OFF);

            printf("\n========== TAG STATUS ==========\n");
            printf("UUID      : %02X%02X%02X%02X-%02X%02X-%02X%02X-"
                   "%02X%02X-%02X%02X%02X%02X%02X%02X\n",
                   SYSTEM_UUID[0],  SYSTEM_UUID[1],
                   SYSTEM_UUID[2],  SYSTEM_UUID[3],
                   SYSTEM_UUID[4],  SYSTEM_UUID[5],
                   SYSTEM_UUID[6],  SYSTEM_UUID[7],
                   SYSTEM_UUID[8],  SYSTEM_UUID[9],
                   SYSTEM_UUID[10], SYSTEM_UUID[11],
                   SYSTEM_UUID[12], SYSTEM_UUID[13],
                   SYSTEM_UUID[14], SYSTEM_UUID[15]);
            printf("Major     : 0x%04X (%s)\n", TAG_MAJOR,
                   TAG_MAJOR == 0x0001 ? "PERSON" : "ASSET");
            printf("Minor     : 0x%04X  (Tag ID = %u)\n",
                   TAG_MINOR, TAG_MINOR);
            printf("TX Power  : %d dBm  (measured power ref — calibrate!)\n",
                   TAG_TX_POWER);
            printf("Sequence  : %u\n",    g_sequence);
            printf("CRC-16    : 0x%04X\n", p->crc16);
            printf("ADV state : %s\n", g_adv_active ? "ACTIVE" : "STOPPED");
            printf("Heap free : %lu bytes\n",
                   (unsigned long)esp_get_free_heap_size());
            printf("================================\n");
        }
    }
}

/* ==========================================================================
 * MAIN
 * ========================================================================== */
void app_main(void)
{
    ESP_LOGI(TAG, "=== BLE Tag Starting ===");
    ESP_LOGI(TAG, "Major=0x%04X (%s)  Minor=0x%04X  TXPwrRef=%ddBm",
             TAG_MAJOR,
             TAG_MAJOR == 0x0001 ? "PERSON" : "ASSET",
             TAG_MINOR, TAG_TX_POWER);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(bluetooth_init());
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));

    /* Sequence timer: 500ms auto-reload
     * Mỗi lần fire: sequence++, restart ADV với interval mới */
    g_seq_timer = xTimerCreate("seq", pdMS_TO_TICKS(500),
                                pdTRUE, NULL, seq_timer_cb);
    xTimerStart(g_seq_timer, 0);

    /* Bắt đầu advertise */
    start_adv_random_interval();

    xTaskCreate(uart_task, "uart", 2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "=== Tag READY (1=status) ===");
}