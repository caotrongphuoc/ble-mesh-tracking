#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
	const char* url;                 /* URL of the .bin on the OTA HTTP server */
	const char* wifi_ssid;           /* WiFi SSID (used only for OTA) */
	const char* wifi_pass;           /* WiFi password */
	const char* nvs_namespace;       /* NVS namespace for the "OTA just succeeded, must report to Gateway" flag */
	uint32_t wifi_timeout_ms;        /* WiFi-connect timeout (default 30000) */
	uint32_t auto_check_interval_ms; /* Version self-check interval (default 180000) */
	bool silence_log_during_ota;     /* true = mute log level to NONE during OTA (scanner) */
} bmt_ota_config_t;

/* Call once at boot BEFORE any other bmt_ota_* function. The config is
 * copied into a static local, so the caller does not need to keep the
 * struct alive. */
void bmt_ota_init(const bmt_ota_config_t* cfg);

/* Start WiFi OTA if it is not already running (idempotent) — called from
 * bmt_mesh.c when OTA_TRIGGER arrives over mesh, or from UART / beacon. */
void bmt_ota_trigger(void);

/* true while OTA is in progress — bmt_scan uses it to pause GAP scan
 * (hands the whole radio to WiFi); bmt_uart uses it to display status. */
bool bmt_ota_is_triggered(void);

/* Call once at boot (after bmt_ota_init + the mesh is up) — if the NVS
 * flag "OTA just succeeded and the reboot completed" is set, this task
 * reports SUCCESS back to the Gateway. */
void bmt_ota_start_pending_report_task(void);

/* Background task that polls the server's firmware version periodically
 * and self-triggers OTA if it differs. Does not need a Gateway or UART
 * trigger. Call once at boot. */
void bmt_ota_start_auto_check(void);
