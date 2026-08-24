#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Initialise the battery ADC. On ProMicro / nice!nano v2 the battery
 * wires directly into the VDDH pin, read via the SAADC's internal
 * VDDHDIV5 input - no external divider, no GPIO enable needed (very
 * different from XIAO). See bmt_battery.c. */
int bmt_battery_init(void);

/* Read the actual battery voltage at VDDH (compensating for the /5
 * ratio of VDDHDIV5), in mV. Returns < 0 on error. */
int bmt_battery_read_mv(void);

/* Estimate remaining battery percentage from voltage using a Li-ion
 * lookup curve (interpolated, see BATT_CURVE in bmt_battery.c). 0-100. */
uint8_t bmt_battery_percent(int mv);

/* This board does not expose a charging-status GPIO, so this always
 * returns false. Kept for API parity with the XIAO variant (bmt_beacon
 * shares the same headers). */
bool bmt_battery_is_charging(void);

/* Cached % from the most recent periodic read - used by the beacon
 * to embed in each ADV payload without re-reading the ADC. Refreshed
 * by a 30 s timer in bmt_battery.c and once synchronously at init. */
uint8_t bmt_battery_last_percent(void);
