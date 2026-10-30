#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct
{
	const char* url;       /* URL of the .bin on the OTA HTTPS server */
	const char* server_cn; /* Common Name to verify on the server cert (e.g. "bmt-tb.local") */
	uint32_t timeout_ms;   /* HTTP timeout */
	bool keep_alive;       /* HTTP keep-alive */
} bmt_https_ota_cfg_t;

/* Download the image from cfg->url (server verified against the embedded
 * ota_ca.pem + cfg->server_cn) and flash it, unless its ELF SHA256 matches
 * the running image or its version is not newer (no downgrade).
 * Does not reboot.
 * Return: ESP_OK = flashed; ESP_ERR_NOT_FOUND = skipped (not an error);
 * other = real failure. */
esp_err_t bmt_https_ota_fetch_and_flash(const bmt_https_ota_cfg_t* cfg);
