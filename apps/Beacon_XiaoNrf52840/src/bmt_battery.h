#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Initialise the on-board battery-measurement path (P0.14 enable
 * + P0.31 ADC via the on-board voltage divider) for a cell wired
 * to BAT+ / BAT- (LIR2032 / LiPo). See bmt_battery.c for details. */
int bmt_battery_init(void);

/* Read the actual battery voltage at BAT+ (compensating for the
 * divider ratio), in mV. Returns < 0 on error. */
int bmt_battery_read_mv(void);

/* Estimate remaining battery percentage from voltage using a Li-ion
 * lookup curve (interpolated, see BATT_CURVE in bmt_battery.c). 0-100. */
uint8_t bmt_battery_percent(int mv);

/* true when the charger IC is charging (P0.17 LOW). */
bool bmt_battery_is_charging(void);

/* Cached % from the most recent periodic read - used by the beacon
 * to embed in each ADV payload without re-reading the ADC. Refreshed
 * by a 30 s timer in bmt_battery.c and once synchronously at init. */
uint8_t bmt_battery_last_percent(void);
