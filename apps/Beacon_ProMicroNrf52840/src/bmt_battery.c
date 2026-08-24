#include "bmt_battery.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bmt_battery, LOG_LEVEL_INF);

/* Battery wires directly into nRF52840's VDDH (nice!nano v2
 * high-voltage mode). Read via the SAADC's internal VDDHDIV5 - 
 * no external divider and no GPIO enable needed (unlike XIAO,
 * which needs P0.31 + P0.14). */
static const struct adc_dt_spec battery_adc = ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);

/* VDDHDIV5: raw reading = VDDH / 5, so multiply by 5 to recover the
 * actual battery voltage. Replaces the (R1+R2)/R2 external-divider
 * ratio used on XIAO. */
#define BMT_BATT_VDDH_DIVIDER 5

/* Voltage -> percent lookup for Li-ion / LiPo. The Li-ion SoC-vs-
 * voltage curve is strongly non-linear, so a single linear formula
 * would be wrong across the range - use a table and interpolate
 * between adjacent points. 4200 mV = 100% (full), 3300 mV = 0%
 * (safe lower cutoff; below this the cell degrades). Identical to
 * the XIAO variant - same battery chemistry, same curve. */
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

	/* Multiply by the VDDHDIV5 ratio to recover the real VDDH voltage. */
	return adc_mv * BMT_BATT_VDDH_DIVIDER;
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
	/* This board does NOT route the charger IC's ~CHG pin to any
	 * readable GPIO - unlike XIAO (P0.17). Return false to keep the
	 * API parity with the XIAO variant.
	 *
	 * Could be inferred indirectly from VDDH itself: when USB is
	 * connected, VDDH climbs to ~5 V, well above a full battery
	 * (4.2 V). Not implemented - need a real board to calibrate
	 * the threshold first. */
	return false;
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
	LOG_INF("[BATTERY] VDDH=%d mV (~%u%%)", mv, s_last_percent);
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
	if (!adc_is_ready_dt(&battery_adc))
	{
		LOG_ERR("ADC device %s not ready", battery_adc.dev->name);
		return -ENODEV;
	}

	int err = adc_channel_setup_dt(&battery_adc);
	if (err < 0)
	{
		LOG_ERR("adc_channel_setup_dt failed (%d)", err);
		return err;
	}

	LOG_INF("Battery ADC (internal VDDHDIV5) ready");

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
