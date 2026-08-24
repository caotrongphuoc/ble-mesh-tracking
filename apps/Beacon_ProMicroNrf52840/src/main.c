#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/drivers/gpio.h>

#include "bmt_auth.h"
#include "bmt_beacon.h"
#include "bmt_battery.h"

LOG_MODULE_REGISTER(bmt_main, LOG_LEVEL_INF);

/* [POWER] Soft switch that cuts power to the external VCC pin
 * (P0.13) - full explanation in
 * boards/promicro_nrf52840_nrf52840_uf2.overlay.
 * Summary: the 3.3 V branch routed to the external pin is powered
 * by default (Zephyr does not declare a control pin, so it stays
 * in its reset state), wasting ~1 mA even when the app powers
 * nothing external. */
static const struct gpio_dt_spec ext_vcc =
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), ext_vcc_gpios);

static void ext_vcc_off(void)
{
	if (!gpio_is_ready_dt(&ext_vcc))
	{
		LOG_ERR("ext-vcc GPIO not ready");
		return;
	}

	/* INACTIVE + active-high = drive LOW = MOSFET off.
	 * Safe for the MCU: this MOSFET only sits on the external VCC
	 * rail; the nRF52840 itself is powered separately via VDDH. */
	int err = gpio_pin_configure_dt(&ext_vcc, GPIO_OUTPUT_INACTIVE);
	if (err)
	{
		LOG_ERR("Failed to turn off external VCC (err %d)", err);
		return;
	}
	LOG_INF("External VCC pin (P0.13) turned off");
}

static void bt_ready(int err)
{
	if (err)
	{
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return;
	}
	LOG_INF("Bluetooth initialized");

	/* MUST be called before bmt_beacon_start() - the beacon needs
	 * the HMAC key ready so it can compute mac16 for the very
	 * first ADV. */
	bmt_auth_init();

	err = bmt_beacon_start();
	if (err)
	{
		LOG_ERR("bmt_beacon_start failed (err %d)", err);
		return;
	}
	LOG_INF("BMT Tag beacon started");
}

int main(void)
{
	printk("=== BMT Tag (nRF52840) starting ===\n");

	/* As EARLY as possible - every millisecond of delay lets current
	 * waste on the external VCC rail. */
	ext_vcc_off();

	/* Battery reading has nothing to do with BLE, start it in
	 * parallel with bt_enable. */
	int batt_err = bmt_battery_init();
	if (batt_err)
	{
		LOG_ERR("bmt_battery_init failed (err %d)", batt_err);
	}

	int err = bt_enable(bt_ready);
	if (err)
	{
		LOG_ERR("bt_enable failed (err %d)", err);
	}
	return 0;
}
