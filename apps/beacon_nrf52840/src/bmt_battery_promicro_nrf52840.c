#include "bmt_battery.h"
#include "bmt_battery_board.h"

/* ProMicro nRF52840 / nice!nano v2: the battery wires directly into
 * the nRF52840's VDDH (high-voltage mode). Read via the SAADC's
 * internal VDDHDIV5 (see the overlay) - no external divider and no
 * GPIO enable needed (unlike XIAO, which needs P0.31 + P0.14). */
const char* const bmt_battery_board_rail = "VDDH";

/* VDDHDIV5: raw reading = VDDH / 5, so multiply by 5 to recover the
 * actual battery voltage. */
#define BMT_BATT_VDDH_DIVIDER 5

int bmt_battery_board_init(void)
{
	return 0;
}

int bmt_battery_board_adc_to_mv(int32_t adc_mv)
{
	return adc_mv * BMT_BATT_VDDH_DIVIDER;
}

bool bmt_battery_is_charging(void)
{
	/* This board does NOT route the charger IC's ~CHG pin to any
	 * readable GPIO - unlike XIAO (P0.17).
	 *
	 * Could be inferred indirectly from VDDH itself: when USB is
	 * connected, VDDH climbs to ~5 V, well above a full battery
	 * (4.2 V). Not implemented - need a real board to calibrate
	 * the threshold first. */
	return false;
}
