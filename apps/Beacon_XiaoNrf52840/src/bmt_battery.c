#include "bmt_battery.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bmt_battery, LOG_LEVEL_INF);

/* [POWER] Read the battery over the BAT+ / BAT- rail (LIR2032 3.6 V
 * or LiPo).
 *
 * Why NOT read VDD directly: when the cell feeds through the
 * on-board regulator, VDD is pinned at ~3.3 V regardless of whether
 * the cell is at 4.2 V or 3.4 V - so reading VDD tells us nothing
 * useful. Read the voltage at the BAT+ node through the on-board
 * 1M/510k divider brought out to P0.31 (AIN7). Formula and resistor
 * values follow the Tjoms99/xiao_sense_nrf52840_battery_lib
 * community library (verified on real hardware).
 *
 * (If later switching back to a CR2032 wired straight to 3V3,
 * change this to read NRF_SAADC_VDD in the overlay and drop the
 * divider - different supply, different measurement path.) */
static const struct adc_dt_spec battery_adc = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);

/* On-board divider: BAT+ --[R1]-- P0.31 --[R2]-- (P0.14 pulls GND).
 * V_bat = V_adc * (R1 + R2) / R2. Nominal 1M / 510k; R1 is set to
 * 1037k per the empirical calibration from the reference library
 * (resistor tolerance). For higher accuracy, measure real V_bat
 * with a DMM and recompute R1. */
#define BMT_BATT_R1_KOHM 1037
#define BMT_BATT_R2_KOHM 510

/* P0.14 = VBAT_ENABLE (active-low): drive LOW to close the divider
 * into the measurement path.
 * P0.17 = charge status (LOW while charging). */
static const struct device* gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
#define BMT_BATT_ENABLE_PIN 14
#define BMT_BATT_CHARGE_STAT_PIN 17

/* Voltage -> percent lookup for Li-ion (LIR2032 / LiPo). The Li-ion
 * SoC-vs-voltage curve is strongly non-linear, so a single linear
 * formula would be wrong across the range - use a table and
 * interpolate between adjacent points. 4200 mV = 100% (full),
 * 3300 mV = 0% (safe cutoff; below this the cell degrades).
 * Curve from the Tjoms99 library and the Nordic SoC blog. */
typedef struct
{
	uint16_t mv;
	uint8_t pct;
} batt_point_t;

static const batt_point_t BATT_CURVE[] = {
    {4200, 100},
    {4110, 90},
    {4020, 80},
    {3930, 70},
    {3840, 60},
    {3750, 50},
    {3660, 40},
    {3570, 30},
    {3480, 20},
    {3390, 10},
    {3300, 0},
};
#define BATT_CURVE_N (sizeof(BATT_CURVE) / sizeof(BATT_CURVE[0]))

#define BMT_BATT_LOG_INTERVAL_SEC 30

static struct k_timer batt_log_timer;
static struct k_work batt_log_work;

/* Cached % - the beacon reads this and embeds it in each ADV
 * (instead of hitting the ADC per packet). Refreshed by the work
 * handler (30 s) and once synchronously during init. */
static uint8_t s_last_percent = 0;

uint8_t bmt_battery_last_percent(void)
{
	return s_last_percent;
}

int bmt_battery_read_mv(void)
{
	int16_t raw = 0;
	struct adc_sequence sequence = {
	    .buffer = &raw,
	    .buffer_size = sizeof(raw),
	};

	int err = adc_sequence_init_dt(&battery_adc, &sequence);
	if (err < 0)
	{
		LOG_ERR("adc_sequence_init_dt failed (%d)", err);
		return err;
	}

	err = adc_read_dt(&battery_adc, &sequence);
	if (err < 0)
	{
		LOG_ERR("adc_read_dt failed (%d)", err);
		return err;
	}

	int32_t adc_mv = raw;
	err = adc_raw_to_millivolts_dt(&battery_adc, &adc_mv);
	if (err < 0)
	{
		LOG_ERR("adc_raw_to_millivolts_dt failed (%d)", err);
		return err;
	}

	/* Reverse the divider ratio to recover the real BAT+ voltage. */
	int32_t vbat = (adc_mv * (BMT_BATT_R1_KOHM + BMT_BATT_R2_KOHM)) / BMT_BATT_R2_KOHM;
	return vbat;
}

uint8_t bmt_battery_percent(int mv)
{
	if (mv >= BATT_CURVE[0].mv)
		return 100;
	if (mv <= BATT_CURVE[BATT_CURVE_N - 1].mv)
		return 0;

	for (size_t i = 0; i < BATT_CURVE_N - 1; i++)
	{
		uint16_t hi = BATT_CURVE[i].mv;
		uint16_t lo = BATT_CURVE[i + 1].mv;
		if (mv <= hi && mv >= lo)
		{
			/* Linear interpolation between the two adjacent points. */
			int pct_hi = BATT_CURVE[i].pct;
			int pct_lo = BATT_CURVE[i + 1].pct;
			return (uint8_t)(pct_lo + ((mv - lo) * (pct_hi - pct_lo)) / (hi - lo));
		}
	}
	return 0;
}

bool bmt_battery_is_charging(void)
{
	/* P0.17 LOW = charging (charger IC pulls it down). */
	return gpio_pin_get(gpio0, BMT_BATT_CHARGE_STAT_PIN) == 0;
}

static void batt_log_work_handler(struct k_work* work)
{
	ARG_UNUSED(work);

	int mv = bmt_battery_read_mv();
	if (mv < 0)
	{
		LOG_ERR("Battery read failed (%d)", mv);
		return;
	}
	s_last_percent = bmt_battery_percent(mv);
	LOG_INF("[BATTERY] BAT+=%d mV (~%u%%) %s", mv, s_last_percent,
	        bmt_battery_is_charging() ? "[CHARGING]" : "");
}

static void batt_log_timer_handler(struct k_timer* timer)
{
	/* k_timer callback runs in ISR context; adc_read may block,
	 * so hand it off to the workqueue. */
	ARG_UNUSED(timer);
	k_work_submit(&batt_log_work);
}

int bmt_battery_init(void)
{
	if (!device_is_ready(gpio0))
	{
		LOG_ERR("gpio0 not ready");
		return -ENODEV;
	}

	/* P0.14 VBAT_ENABLE active-low: OUTPUT_ACTIVE -> drive LOW ->
	 * close the divider into the measurement path. Keep LOW for
	 * the whole lifetime (see the overlay note) - MUST NOT be
	 * HIGH (even briefly): Seeed officially warns that P0.31 can
	 * overshoot the ADC input limit when P0.14 is HIGH. */
	int err = gpio_pin_configure(gpio0, BMT_BATT_ENABLE_PIN,
	                             GPIO_OUTPUT_ACTIVE | GPIO_ACTIVE_LOW);
	if (err < 0)
	{
		LOG_ERR("config VBAT_ENABLE (P0.14) failed (%d)", err);
		return err;
	}

	/* P0.17 charge-status: INPUT to read the charge state. */
	err = gpio_pin_configure(gpio0, BMT_BATT_CHARGE_STAT_PIN, GPIO_INPUT);
	if (err < 0)
	{
		LOG_ERR("config CHARGE_STAT (P0.17) failed (%d)", err);
		return err;
	}

	if (!adc_is_ready_dt(&battery_adc))
	{
		LOG_ERR("ADC device %s not ready", battery_adc.dev->name);
		return -ENODEV;
	}

	err = adc_channel_setup_dt(&battery_adc);
	if (err < 0)
	{
		LOG_ERR("adc_channel_setup_dt failed (%d)", err);
		return err;
	}

	LOG_INF("Battery ADC (BAT+ on-board divider) ready");

	/* Synchronous read once so s_last_percent has a valid value
	 * BEFORE the beacon starts advertising (build_adv_data reads
	 * bmt_battery_last_percent). */
	int mv0 = bmt_battery_read_mv();
	if (mv0 >= 0)
		s_last_percent = bmt_battery_percent(mv0);

	k_work_init(&batt_log_work, batt_log_work_handler);
	k_timer_init(&batt_log_timer, batt_log_timer_handler, NULL);
	k_timer_start(&batt_log_timer, K_SECONDS(BMT_BATT_LOG_INTERVAL_SEC),
	              K_SECONDS(BMT_BATT_LOG_INTERVAL_SEC));

	/* Print once at boot as well. */
	k_work_submit(&batt_log_work);

	return 0;
}
