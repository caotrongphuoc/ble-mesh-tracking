#pragma once

#include <stdint.h>

#include "esp_ble_mesh_defs.h"
#define BMT_CID_ESP 0x02E5
#define BMT_VND_MODEL_ID 0x0000

/* Opcodes must match Scanner and Relay byte-for-byte. */
#define BMT_OP_VND_TAG_STATUS ESP_BLE_MESH_MODEL_OP_3(0x00, BMT_CID_ESP)
#define BMT_OP_VND_RESET_CMD ESP_BLE_MESH_MODEL_OP_3(0x05, BMT_CID_ESP)
#define BMT_OP_VND_OTA_TRIGGER ESP_BLE_MESH_MODEL_OP_3(0x06, BMT_CID_ESP)  /* WiFi OTA command */
#define BMT_OP_VND_OTA_RESULT ESP_BLE_MESH_MODEL_OP_3(0x07, BMT_CID_ESP)   /* node reports OTA success/failure */
#define BMT_OP_VND_OTA_KEY_PUSH ESP_BLE_MESH_MODEL_OP_3(0x08, BMT_CID_ESP) /* [SECURITY] Gateway pushes new OTA-beacon HMAC key (16 bytes) — already encrypted by mesh AppKey */

#define BMT_NODE_TYPE_SCANNER 0x01
#define BMT_NODE_TYPE_RELAY 0x02
#define BMT_NODE_TYPE_ALL 0xFF

#pragma pack(1)
typedef struct
{
	uint8_t scanner_id;
	uint8_t battery; /* tag battery % (0-100). Was tag_type before. MUST match
	                  * the Scanner-side struct (apps/scanner bmt_types.h) byte
	                  * for byte. */
	uint16_t tag_id;
	int8_t rssi;
	int16_t distance_dm;
	uint8_t loss_pct;
} bmt_tag_report_t;

typedef struct
{
	uint8_t status; /* 0 = OTA success, non-zero = failure (see node log for the reason) */
} bmt_ota_result_t;
#pragma pack()
