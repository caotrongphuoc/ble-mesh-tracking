#pragma once

#include <stdint.h>

#include "bmt_protocol_mesh.h"

#define BMT_NODE_TYPE BMT_NODE_TYPE_SCANNER /* used to filter the target in the OTA-beacon */

/* iPhone-as-tag (Apple iBeacon) */
#define BMT_CID_APPLE 0x004C
#define BMT_TAG_MAJOR_PERSON 0x0001
#define BMT_TAG_MAJOR_ASSET 0x0002
#define BMT_PHONE_TX_POWER_1M (-59)

/* Decoded tag ADV, common to our tags and iPhone iBeacons. */
#pragma pack(1)
typedef struct
{
	uint8_t battery; /* tag battery % (0-100) */
	uint16_t tag_id;
	int8_t tx_power;
	uint8_t sequence;
	uint16_t mac16;
} bmt_tag_payload_t;
#pragma pack()
