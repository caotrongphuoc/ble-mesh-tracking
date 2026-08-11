#pragma once

#include <stdint.h>

#include "esp_ble_mesh_defs.h"

#define BMT_CID_ESP 0x02E5
#define BMT_VND_MODEL_ID 0x0000

/* Opcodes must match Gateway and Scanner byte-for-byte. */
#define BMT_OP_VND_RESET_CMD ESP_BLE_MESH_MODEL_OP_3(0x05, BMT_CID_ESP)
#define BMT_OP_VND_OTA_TRIGGER ESP_BLE_MESH_MODEL_OP_3(0x06, BMT_CID_ESP) /* WiFi OTA command */
#define BMT_OP_VND_OTA_RESULT ESP_BLE_MESH_MODEL_OP_3(0x07, BMT_CID_ESP)  /* node reports OTA success/failure */

#pragma pack(1)
typedef struct
{
	uint8_t status; /* 0 = OTA success, non-zero = failure (see node log for the reason) */
} bmt_ota_result_t;
#pragma pack()
