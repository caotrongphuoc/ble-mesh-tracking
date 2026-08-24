#pragma once

#include <stdint.h>

/* MUST match the ESP32 struct byte-for-byte
 * (apps/tag/components/bmt_beacon/bmt_beacon.h). This is the only
 * interface between Tag and the rest of the system (Scanner / Mesh /
 * Gateway). A single-byte drift makes the Scanner refuse to recognise
 * the tag. */
#pragma pack(1)
typedef struct
{
	uint8_t uuid[16];    /* 16B: system UUID (AB000000-...) */
	uint16_t battery;    /*  2B: remaining battery % (0-100). Was "major"
	                      *      (PERSON/ASSET) - dropped because worn
	                      *      tags don't need the classification.
	                      *      Position and size kept, so HMAC and the
	                      *      24 B layout are unchanged. */
	uint16_t minor;      /*  2B: tag ID within the system */
	int8_t tx_power;     /*  1B: measured power at 1 m (calibrated) */
	uint8_t sequence;    /*  1B: 0-255, wraps -> Scanner computes loss rate */
	uint16_t mac16;      /*  2B: truncated HMAC-SHA256 (replaces CRC16) */
} bmt_tag_adv_payload_t; /* = 24 bytes */
#pragma pack()
