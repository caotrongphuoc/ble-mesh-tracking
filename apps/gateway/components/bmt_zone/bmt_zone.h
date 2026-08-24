#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BMT_MAX_SCANNERS 8
#define BMT_MAX_TRACKED_TAGS 16
#define BMT_SCANNER_VALID_MS 3500
#define BMT_TAG_OUT_OF_RANGE_MS 10000
#define BMT_ZONE_UNKNOWN 0xFF

typedef struct
{
	bool active;
	uint16_t tag_id;
	uint8_t battery; /* latest battery % (0-100). Was tag_type before. */
	int8_t rssi_by_scanner[BMT_MAX_SCANNERS];
	uint32_t ts_by_scanner[BMT_MAX_SCANNERS];
	bool valid_by_scanner[BMT_MAX_SCANNERS];
	uint8_t current_zone_id;
	bool out_of_range_pending;
	uint32_t last_zone_change_ms, last_any_report_ms;
} bmt_tag_track_t;

/* Initialise the mutex that guards the tag-track table. Call once from
 * app_main before any task that might call another function in this
 * module. */
void bmt_zone_init(void);

/* Take/give the mutex that guards the entire bmt_tag_track_t table. The
 * caller MUST bundle related operations (find / get_or_add + update
 * field + evaluate) into a single lock / unlock pair; do not hold a
 * pointer across the boundary. */
void bmt_zone_lock(void);
void bmt_zone_unlock(void);

int bmt_zone_track_capacity(void);
bmt_tag_track_t* bmt_zone_track_get(int idx);
bmt_tag_track_t* bmt_zone_track_find(uint16_t tag_id);
/* Add a new entry if tag_id is not tracked yet. Returns NULL if the
 * table is full. */
bmt_tag_track_t* bmt_zone_track_get_or_add(uint16_t tag_id, uint8_t battery);

const char* bmt_zone_name(uint8_t scanner_id);

/* UART '2' - print the tracked-tag table and the current zone. */
void bmt_zone_log_tracked(void);

/* Wipe the entire tag-track table. Used by UART '0' / '9' and by
 * bmt_watchdog.c when the gateway does a full reset. */
void bmt_zone_reset_all(void);
