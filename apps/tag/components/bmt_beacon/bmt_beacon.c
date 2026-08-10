#include "bmt_beacon.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"

#include "bmt_config.h"
#include "bmt_auth.h"

static const char* TAG = "BMT_BEACON";
static uint8_t s_sequence = 0;
static TimerHandle_t s_seq_timer = NULL;
static bool s_adv_active = false;
#define ADV_RAW_LEN 31
#define ADV_PAYLOAD_OFF 7

static uint8_t s_adv_raw[ADV_RAW_LEN] = {
    /* Flags */
    0x02,
    0x01,
    0x06,
    /* Manufacturer Specific Data header */
    0x1B,
    0xFF,
    0xE5,
    0x02,
    /* 24 bytes payload (filled by build_adv_data) */
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
};
static void build_adv_data(void)
{
	bmt_tag_adv_payload_t p;

	memcpy(p.uuid, BMT_SYSTEM_UUID, 16);
	p.major = BMT_TAG_MAJOR;
	p.minor = BMT_TAG_MINOR;
	p.tx_power = BMT_TAG_TX_POWER;
	p.sequence = s_sequence;
	p.mac16 = 0; /* must be 0 before computing HMAC */

	/* HMAC covers every field except the last 2 mac16 bytes. */
	p.mac16 = bmt_auth_hmac16((uint8_t*)&p, sizeof(p) - sizeof(p.mac16));

	memcpy(s_adv_raw + ADV_PAYLOAD_OFF, &p, sizeof(p));
}
static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0,
    .adv_int_max = 0,
    .adv_type = ADV_TYPE_NONCONN_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .peer_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void start_adv_random_interval(void)
{
	/* Random [450, 550] ms -> avoid collision with other tags. */
	uint32_t ms = BMT_ADV_INTERVAL_MIN_MS + (esp_random() % (BMT_ADV_INTERVAL_MAX_MS - BMT_ADV_INTERVAL_MIN_MS + 1));

	/* BLE unit = 0.625ms */
	uint16_t units = (uint16_t)((ms * 1000) / 625);
	s_adv_params.adv_int_min = units;
	s_adv_params.adv_int_max = units;

	build_adv_data();

	/* config_adv_data_raw → GAP callback → start_advertising */
	esp_ble_gap_config_adv_data_raw(s_adv_raw, ADV_RAW_LEN);
}
static void seq_timer_cb(TimerHandle_t xTimer)
{
	(void)xTimer;
	s_sequence++; /* uint8_t: 255 -> 0 automatically */

	esp_ble_gap_stop_advertising();
	start_adv_random_interval();
}
static void gap_event_handler(esp_gap_ble_cb_event_t event,
                              esp_ble_gap_cb_param_t* param)
{
	switch (event)
	{
	case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
		if (param->adv_data_raw_cmpl.status == ESP_BT_STATUS_SUCCESS)
			esp_ble_gap_start_advertising(&s_adv_params);
		else
			ESP_LOGE(TAG, "ADV data set FAILED");
		break;

	case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
		if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS)
		{
			s_adv_active = true;
			ESP_LOGD(TAG, "ADV OK seq=%u major=0x%04X minor=0x%04X",
			         s_sequence, BMT_TAG_MAJOR, BMT_TAG_MINOR);
		}
		else
		{
			ESP_LOGE(TAG, "ADV start FAILED");
		}
		break;

	case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
		s_adv_active = false;
		break;

	default:
		break;
	}
}
static esp_err_t bluetooth_init(void)
{
	ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

	esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
	ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

	esp_bluedroid_config_t cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&cfg));
	ESP_ERROR_CHECK(esp_bluedroid_enable());

	/* Set radio TX power — affects range and battery life. */
	esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, BMT_TAG_RADIO_PWR);

	return ESP_OK;
}
esp_err_t bmt_beacon_start(void)
{
	esp_err_t err = bluetooth_init();
	if (err != ESP_OK)
		return err;

	err = esp_ble_gap_register_callback(gap_event_handler);
	if (err != ESP_OK)
		return err;

	/* Sequence timer: 500 ms auto-reload.
	 * Each fire: sequence++, restart ADV with a new interval. */
	s_seq_timer = xTimerCreate("seq", pdMS_TO_TICKS(500),
	                           pdTRUE, NULL, seq_timer_cb);
	xTimerStart(s_seq_timer, 0);

	/* Start advertising. */
	start_adv_random_interval();

	return ESP_OK;
}

uint8_t bmt_beacon_sequence(void)
{
	return s_sequence;
}
bool bmt_beacon_is_active(void)
{
	return s_adv_active;
}

uint16_t bmt_beacon_last_mac16(void)
{
	/* Copy into an aligned local — s_adv_raw + ADV_PAYLOAD_OFF may sit at an odd address. */
	bmt_tag_adv_payload_t p;
	memcpy(&p, s_adv_raw + ADV_PAYLOAD_OFF, sizeof(p));
	return p.mac16;
}
