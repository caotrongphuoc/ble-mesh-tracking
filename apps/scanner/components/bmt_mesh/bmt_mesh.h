#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Provisioning state, OTA result report and local reset (bmt_mesh_*). */
#include "bmt_mesh_node.h"

/* Build UUID from MAC and scanner_id, then start the shared mesh node
 * (restores provisioning from NVS or enables fresh provisioning). */
esp_err_t bmt_mesh_init(void);

/* Pointer to the current 16-byte UUID (SCAN + MAC + ... + scanner_id).
 * Read-only: use for log/status; do not modify directly. */
const uint8_t* bmt_mesh_uuid(void);

/* Walk every active tag in bmt_tag_table and publish TAG_STATUS to mesh. */
void bmt_mesh_publish_tags(void);
