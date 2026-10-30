#include "bmt_battery.h"
#include "bmt_battery_board.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(bmt_battery);

/* [POWER] XIAO nRF52840: read the battery over the BAT+ / BAT- rail
 * (LIR2032 3.6 V or LiPo).
 *
 * Why NOT read VDD directly: when the cell feeds through the
 * on-board regulator, VDD is pinned at ~3.3 V regardless of whether
 * the cell is at 4.2 V or 3.4 V - so reading VDD tells us nothing
 * useful. Read the voltage at the BAT+ node through the on-board
 * 1M/510k divider brought out to P0.31 (AIN7, see the overlay).
 * Formula and resistor values follow the
 * Tjoms99/xiao_sense_nrf52840_battery_lib community library
 * (verified on real hardware).
 *
 * (If later switching back to a CR2032 wired straight to 3V3,
 * change this to read NRF_SAADC_VDD in the overlay and drop the
 * divider - different supply, different measurement path.) */
const char* const bmt_battery_board_rail = "BAT+";

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

int bmt_battery_board_init(void)
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
	return 0;
}

int bmt_battery_board_adc_to_mv(int32_t adc_mv)
{
	/* Reverse the divider ratio to recover the real BAT+ voltage. */
	return (adc_mv * (BMT_BATT_R1_KOHM + BMT_BATT_R2_KOHM)) / BMT_BATT_R2_KOHM;
}

bool bmt_battery_is_charging(void)
{
	/* P0.17 LOW = charging (charger IC pulls it down). */
	return gpio_pin_get(gpio0, BMT_BATT_CHARGE_STAT_PIN) == 0;
}
