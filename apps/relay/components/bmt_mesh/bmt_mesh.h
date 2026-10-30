#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* Provisioning state, OTA result report and local reset (bmt_mesh_*). */
#include "bmt_mesh_node.h"

/* Build the relay composition and start the shared mesh node. */
esp_err_t bmt_mesh_init(void);

bool bmt_mesh_relay_enabled(void);
