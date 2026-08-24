#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BMT_MAX_NODES 10

typedef struct
{
	bool used, is_relay, is_scan, config_done, online;
	uint32_t last_seen_ms;
	uint16_t addr;
	uint8_t uuid[16], mac[6];
	char name[32];
} bmt_node_t;

/* Table capacity - used by callers that want to iterate through
 * bmt_node_table_get(i). */
int bmt_node_table_capacity(void);
bmt_node_t* bmt_node_table_get(int idx);

int bmt_node_table_find(uint16_t addr);
bool bmt_node_table_uuid_provisioned(const uint8_t* uuid);
int bmt_node_table_add(uint16_t addr, const uint8_t* uuid,
                       const uint8_t* mac, const char* name);

void bmt_node_table_load(void);
void bmt_node_table_save(void);
void bmt_node_table_clear(void); /* wipe NVS and zero the in-RAM table */
void bmt_node_table_print(void);

bool bmt_uuid_is_relay(const uint8_t* uuid);
bool bmt_uuid_is_scan(const uint8_t* uuid);
const char* bmt_uuid_type_str(const uint8_t* uuid);
