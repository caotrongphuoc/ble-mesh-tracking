#pragma once

/* Single source of truth for everything that goes over the air between
 * Tag, Scanner, Relay and Gateway. Plain C (no ESP-IDF / Zephyr headers)
 * so both the ESP-IDF apps and the Zephyr nRF52840 beacons include it.
 * Mesh opcodes in ESP-IDF form live in bmt_protocol_mesh.h. */

#include <stdint.h>

/* Espressif Company ID. Used both as the BLE Mesh vendor CID and as the
 * manufacturer-data CID in the tag ADV - Scanner filters on it, the
 * actual chip vendor of the tag (ESP32 or nRF52840) is unrelated. */
#define BMT_CID_ESPRESSIF 0x02E5
#define BMT_VND_MODEL_ID 0x0000

/* Vendor opcode low bytes (3-byte opcode = this byte + CID). */
#define BMT_OPC_TAG_STATUS 0x00
#define BMT_OPC_RESET_CMD 0x05
#define BMT_OPC_OTA_TRIGGER 0x06  /* WiFi OTA command */
#define BMT_OPC_OTA_RESULT 0x07   /* node reports OTA success/failure */
#define BMT_OPC_OTA_KEY_PUSH 0x08 /* [SECURITY] Gateway pushes new OTA-beacon HMAC key (16 bytes) - already encrypted by mesh AppKey */

/* Target filter carried in the Gateway OTA beacon. */
#define BMT_NODE_TYPE_SCANNER 0x01
#define BMT_NODE_TYPE_RELAY 0x02
#define BMT_NODE_TYPE_ALL 0xFF

#pragma pack(1)
/* Tag -> Scanner: manufacturer-data payload after the 2-byte CID. */
typedef struct
{
	uint8_t uuid[16]; /* system UUID (AB000000-...) */
	uint16_t battery; /* remaining battery % (0-100) */
	uint16_t minor;   /* tag ID within the system */
	int8_t tx_power;  /* measured power at 1 m (calibrated) */
	uint8_t sequence; /* 0-255, wraps -> Scanner computes loss rate */
	uint16_t mac16;   /* truncated HMAC-SHA256 over every field above */
} bmt_tag_adv_payload_t;

/* Scanner -> Gateway: BMT_OPC_TAG_STATUS mesh message. */
typedef struct
{
	uint8_t scanner_id;
	uint8_t battery; /* tag battery % (0-100) */
	uint16_t tag_id;
	int8_t rssi;
	int16_t distance_dm;
	uint8_t loss_pct;
} bmt_tag_report_t;

/* Scanner / Relay -> Gateway: BMT_OPC_OTA_RESULT mesh message. */
typedef struct
{
	uint8_t status; /* 0 = OTA success, non-zero = failure (see node log for the reason) */
} bmt_ota_result_t;
#pragma pack()

_Static_assert(sizeof(bmt_tag_adv_payload_t) == 24, "tag ADV payload must stay 24 bytes");
_Static_assert(sizeof(bmt_tag_report_t) == 8, "tag report must stay 8 bytes");
_Static_assert(sizeof(bmt_ota_result_t) == 1, "OTA result must stay 1 byte");
