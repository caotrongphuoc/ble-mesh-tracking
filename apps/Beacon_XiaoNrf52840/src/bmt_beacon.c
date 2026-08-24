#include "bmt_beacon.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/logging/log.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>

#include "bmt_config.h"
#include "bmt_auth.h"
#include "bmt_types.h"
#include "bmt_battery.h"

LOG_MODULE_REGISTER(bmt_beacon, LOG_LEVEL_INF);

/* Espressif Company ID (0x02E5) - kept so existing Scanners (ESP32)
 * still recognise the tag, even though the Tag now runs a Nordic
 * chip. Scanner filters by this CID in the manufacturer data; the
 * actual chip vendor is unrelated. */
#define BMT_CID_ESPRESSIF 0x02E5

static uint8_t s_sequence = 0;
static bool s_adv_active = false;

/* Manufacturer-data buffer = 2 B CID (little-endian) + 24 B payload.
 * Backed by a static buffer (not stack) because bt_le_adv_start()
 * needs a valid data pointer at call time - same approach as
 * s_adv_raw on ESP32. */
static uint8_t s_mfg_data[2 + sizeof(bmt_tag_adv_payload_t)];

static struct k_timer s_seq_timer;
static struct k_work s_seq_work;

static void build_adv_data(void)
{
	bmt_tag_adv_payload_t p;

	memcpy(p.uuid, BMT_SYSTEM_UUID, 16);
	/* This field used to be major (PERSON / ASSET); now carries
	 * battery % (0-100). Reads the cached value (bmt_battery
	 * refreshes it every 30 s) instead of hitting the ADC on every
	 * ADV - lighter on the battery. */
	p.battery = bmt_battery_last_percent();
	p.minor = BMT_TAG_MINOR;
	p.tx_power = BMT_TAG_TX_POWER;
	p.sequence = s_sequence;
	p.mac16 = 0; /* must be 0 before computing HMAC */

	/* HMAC covers every field except the last 2 mac16 bytes - 
	 * exactly the same formula as ESP32, only the API call differs
	 * (bmt_auth_hmac16 is shared naming). */
	p.mac16 = bmt_auth_hmac16((uint8_t*)&p, sizeof(p) - sizeof(p.mac16));

	s_mfg_data[0] = (uint8_t)(BMT_CID_ESPRESSIF & 0xFF);
	s_mfg_data[1] = (uint8_t)(BMT_CID_ESPRESSIF >> 8);
	memcpy(s_mfg_data + 2, &p, sizeof(p));
}

static void start_adv_random_interval(void)
{
	/* Random [900, 1100] ms -> avoid collision with other tags,
	 * same range as ESP32. */
	uint32_t ms = BMT_ADV_INTERVAL_MIN_MS +
	              (sys_rand32_get() % (BMT_ADV_INTERVAL_MAX_MS - BMT_ADV_INTERVAL_MIN_MS + 1));

	/* One-shot: timer fires exactly after "ms" ms - in sync with
	 * the radio interval currently in use (900-1100 ms). A fixed
	 * 500 ms auto-reload timer would sit in the middle of a radio
	 * cycle and adv_stop / start incorrectly, doubling the average
	 * transmit rate. */
	k_timer_start(&s_seq_timer, K_MSEC(ms), K_NO_WAIT);

	/* BLE unit = 0.625 ms - same unit as ESP-IDF, formula identical. */
	uint16_t units = (uint16_t)((ms * 1000) / 625);

	build_adv_data();

	const struct bt_le_adv_param param = {
	    .id = BT_ID_DEFAULT,
	    .sid = 0,
	    .secondary_max_skip = 0,
	    .options = BT_LE_ADV_OPT_USE_IDENTITY, /* public address, not private / random-rotating */
	    .interval_min = units,
	    .interval_max = units,
	    .peer = NULL,
	};

	const struct bt_data ad[] = {
	    /* Flags = 0x06 (LE General Discoverable | No BR/EDR) - same
	     * hardcoded value as ESP32. */
	    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	    BT_DATA(BT_DATA_MANUFACTURER_DATA, s_mfg_data, sizeof(s_mfg_data)),
	};

	int err = bt_le_adv_start(&param, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err)
	{
		LOG_ERR("ADV start FAILED (err %d)", err);
		s_adv_active = false;
		return;
	}
	s_adv_active = true;
	LOG_DBG("ADV OK seq=%u batt=%u%% minor=0x%04X", s_sequence,
	        bmt_battery_last_percent(), BMT_TAG_MINOR);
}

static void seq_work_handler(struct k_work* work)
{
	ARG_UNUSED(work);
	s_sequence++; /* uint8_t: 255 -> 0 automatically, same as ESP32 */

	bt_le_adv_stop();
	start_adv_random_interval();
}

static void seq_timer_handler(struct k_timer* timer)
{
	/* [IMPORTANT] k_timer callbacks run in ISR context - must NOT
	 * call bt_le_adv_stop / start here (they can block and would
	 * fault). Dispatch the real work to the system workqueue via
	 * k_work, same principle as "FreeRTOS timer callback stays
	 * short, heavy work goes to its own task", but Zephyr enforces
	 * this stricter than ESP32 does. */
	ARG_UNUSED(timer);
	k_work_submit(&s_seq_work);
}

int bmt_beacon_start(void)
{
	k_work_init(&s_seq_work, seq_work_handler);

	/* One-shot: interval is reset inside start_adv_random_interval(). */
	k_timer_init(&s_seq_timer, seq_timer_handler, NULL);

	/* First advertise - this function calls k_timer_start() itself. */
	start_adv_random_interval();

	return 0;
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
	bmt_tag_adv_payload_t p;
	memcpy(&p, s_mfg_data + 2, sizeof(p));
	return p.mac16;
}
