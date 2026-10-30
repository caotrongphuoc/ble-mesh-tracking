#pragma once

#include <stdint.h>

/* Board hooks for bmt_battery.c, implemented by exactly one
 * src/bmt_battery_<board>.c (selected in CMakeLists.txt). */

/* Name of the measured rail, for logs ("BAT+", "VDDH"). */
extern const char* const bmt_battery_board_rail;

/* Board GPIO setup needed before the ADC reads anything. */
int bmt_battery_board_init(void);

/* Convert the ADC pin voltage back to the real battery voltage. */
int bmt_battery_board_adc_to_mv(int32_t adc_mv);
