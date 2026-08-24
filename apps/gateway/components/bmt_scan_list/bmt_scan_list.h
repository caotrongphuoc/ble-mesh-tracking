#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
	BMT_PROV_MODE_AUTO = 0,
	BMT_PROV_MODE_MANUAL = 1
} bmt_prov_mode_t;

bmt_prov_mode_t bmt_scan_list_get_mode(void);
bool bmt_scan_list_is_scanning(void);
bool bmt_scan_list_add(const uint8_t* uuid, const uint8_t* mac,
                       uint8_t addr_type, uint16_t oob_info);

/* UART 's' - switch to MANUAL mode, scan for BMT_SCAN_DURATION_MS
 * (blocks the caller), print the result. */
void bmt_scan_list_start(void);
/* UART 'p' - provision every device in the list that is not already
 * provisioned. */
void bmt_scan_list_provision(void);
void bmt_scan_list_print(void);

/* UART 'a' / 'm' - switch between auto and manual mode. Switching to
 * manual does NOT automatically scan or clear the list; only 's' scans. */
void bmt_scan_list_set_mode(bmt_prov_mode_t mode);
