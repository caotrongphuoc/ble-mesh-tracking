#pragma once

/* ESP-IDF BLE Mesh form of the vendor opcodes in bmt_protocol.h. */

#include "esp_ble_mesh_defs.h"

#include "bmt_protocol.h"

#define BMT_OP_VND_TAG_STATUS ESP_BLE_MESH_MODEL_OP_3(BMT_OPC_TAG_STATUS, BMT_CID_ESPRESSIF)
#define BMT_OP_VND_RESET_CMD ESP_BLE_MESH_MODEL_OP_3(BMT_OPC_RESET_CMD, BMT_CID_ESPRESSIF)
#define BMT_OP_VND_OTA_TRIGGER ESP_BLE_MESH_MODEL_OP_3(BMT_OPC_OTA_TRIGGER, BMT_CID_ESPRESSIF)
#define BMT_OP_VND_OTA_RESULT ESP_BLE_MESH_MODEL_OP_3(BMT_OPC_OTA_RESULT, BMT_CID_ESPRESSIF)
#define BMT_OP_VND_OTA_KEY_PUSH ESP_BLE_MESH_MODEL_OP_3(BMT_OPC_OTA_KEY_PUSH, BMT_CID_ESPRESSIF)

/* [SECURITY] Static OOB value for provisioning - Gateway (provisioner)
 * and every node must use the same 16 bytes. A macro initializer rather
 * than an array so including this header never leaves an unused copy. */
#define BMT_MESH_STATIC_OOB_INIT                            \
	{                                                       \
		0x8E, 0x2F, 0x71, 0xC4, 0x3A, 0x95, 0xD6, 0x0B,     \
		    0x47, 0xE8, 0x1C, 0x63, 0xAF, 0x29, 0x5D, 0x92, \
	}
