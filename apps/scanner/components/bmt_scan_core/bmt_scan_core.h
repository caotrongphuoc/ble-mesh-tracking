#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Call once at the top of app_main(). Orchestrates the full init in
 * the right order: NVS scanner_id -> auth (HMAC key) -> bluetooth
 * -> mesh -> scan (GAP) -> uart. */
esp_err_t bmt_scan_core_init(void);

