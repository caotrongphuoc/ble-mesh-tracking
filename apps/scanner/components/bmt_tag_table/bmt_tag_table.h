#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bmt_types.h"

#define BMT_MAX_TAGS 20
#define BMT_TAG_TIMEOUT_MS 5000
#define BMT_PATH_LOSS_N 2.5f
#define BMT_LOG_RSSI_THRESHOLD_DBM 3
#define BMT_LOG_MIN_INTERVAL_MS 2000

/* [SECURITY] Anti-replay — the maximum sequence jump treated as valid
 * while continuously tracking a tag. See the full explanation in
 * bmt_tag_table.c. */
#define BMT_MAX_SEQ_JUMP 30

typedef struct
{
	bool active;
	uint8_t battery; /* tag battery % (0-100); previously held tag_type PERSON/ASSET */
	uint16_t tag_id;
	int8_t tx_power;
	int8_t rssi_raw;
	float rssi_filtered;
	float distance;
	uint8_t last_sequence;
	uint32_t total_received;
	uint32_t total_missed;
	uint32_t last_seen_ms;
	uint8_t mac[6];
	int8_t last_logged_rssi;
	uint32_t last_log_ms;
	int32_t locked_epoch; /* [SECURITY] epoch the Scanner is currently locked to for this tag; -1 = not yet locked. Display / debug only. */
} bmt_scan_tag_info_t;

void bmt_tag_table_reset(void);

int bmt_tag_table_find(uint16_t tag_id);
int bmt_tag_table_add(uint16_t tag_id, uint8_t battery, int8_t tx_power,
                      int8_t rssi, uint8_t sequence, const uint8_t* mac);
void bmt_tag_table_update(int idx, int8_t rssi, uint8_t sequence, uint8_t battery);
/* Record the epoch the Scanner is currently locked to for the tag at
 * this idx — called from bmt_scan.c right after HMAC verification
 * succeeds. Display / debug only. */
void bmt_tag_table_set_epoch(int idx, int32_t epoch);
void bmt_tag_table_check_timeouts(void);
void bmt_tag_table_print(uint8_t scanner_id);

/* Read-only accessor for other modules (e.g. bmt_mesh.c for publish).
 * Returns NULL if idx is out of range or the tag is not active. */
const bmt_scan_tag_info_t* bmt_tag_table_at(int idx);
