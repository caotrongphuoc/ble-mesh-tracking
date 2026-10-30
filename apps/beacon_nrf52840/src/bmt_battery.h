#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Initialise the board's battery-measurement path and start the
 * periodic read. The ADC channel comes from the zephyr,user node in
 * boards/<board>.overlay; board-specific setup lives in
 * bmt_battery_<board>.c. */
int bmt_battery_init(void);

/* Read the actual battery voltage (compensating for the board's
 * divider), in mV. Returns < 0 on error. */
int bmt_battery_read_mv(void);

/* Estimate remaining battery percentage from voltage using a Li-ion
 * lookup curve (interpolated, see BATT_CURVE in bmt_battery.c). 0-100. */
uint8_t bmt_battery_percent(int mv);

/* true when the charger IC is charging. Always false on boards that
 * do not expose a charge-status pin (ProMicro). */
bool bmt_battery_is_charging(void);

/* Cached % from the most recent periodic read - used by the beacon
 * to embed in each ADV payload without re-reading the ADC. Refreshed
 * by a 30 s timer in bmt_battery.c and once synchronously at init. */
uint8_t bmt_battery_last_percent(void);
