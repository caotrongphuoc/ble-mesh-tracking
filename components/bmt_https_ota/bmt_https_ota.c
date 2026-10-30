#include "bmt_https_ota.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"

/* [SECURITY] CA embedded in firmware to verify the HTTPS OTA server -
 * shares one CA with MQTTS (same physical server). Over plain HTTP
 * anyone on the LAN could download the .bin and read plaintext secrets
 * (WiFi pass, TB token, ...) without touching a board. */
extern const uint8_t bmt_ota_ca_pem_start[] asm("_binary_ota_ca_pem_start");
extern const uint8_t bmt_ota_ca_pem_end[] asm("_binary_ota_ca_pem_end");

static void print_sha256_hex(const char* label, const uint8_t* sha, size_t len)
{
	printf("%s", label);
	for (size_t i = 0; i < len; i++)
		printf("%02x", sha[i]);
	printf("\n");
}

esp_err_t bmt_https_ota_fetch_and_flash(const bmt_https_ota_cfg_t* cfg)
{
	esp_http_client_config_t http_cfg = {
	    .url = cfg->url,
	    .timeout_ms = (int)cfg->timeout_ms,
	    .keep_alive_enable = cfg->keep_alive,
	    .cert_pem = (const char*)bmt_ota_ca_pem_start,
	    .cert_len = (size_t)(bmt_ota_ca_pem_end - bmt_ota_ca_pem_start),
	    .common_name = cfg->server_cn,
	};
	esp_https_ota_config_t ota_cfg = {.http_config = &http_cfg};
	esp_https_ota_handle_t ota_handle = NULL;

	esp_err_t err = esp_https_ota_begin(&ota_cfg, &ota_handle);
	if (err != ESP_OK)
	{
		printf("[OTA] esp_https_ota_begin FAILED: %s\n", esp_err_to_name(err));
		return err;
	}

	esp_app_desc_t new_desc;
	err = esp_https_ota_get_img_desc(ota_handle, &new_desc);
	if (err != ESP_OK)
	{
		printf("[OTA] esp_https_ota_get_img_desc FAILED: %s\n", esp_err_to_name(err));
		esp_https_ota_abort(ota_handle);
		return err;
	}

	const esp_app_desc_t* cur_desc = esp_app_get_description();
	print_sha256_hex("[OTA] Node   SHA256: ", cur_desc->app_elf_sha256, sizeof(cur_desc->app_elf_sha256));
	print_sha256_hex("[OTA] Server SHA256: ", new_desc.app_elf_sha256, sizeof(new_desc.app_elf_sha256));

	if (memcmp(new_desc.app_elf_sha256, cur_desc->app_elf_sha256, sizeof(new_desc.app_elf_sha256)) == 0)
	{
		printf("[OTA] SHA256 match - node firmware is IDENTICAL to server firmware, skip.\n");
		esp_https_ota_abort(ota_handle);
		return ESP_ERR_NOT_FOUND;
	}
	printf("[OTA] SHA256 differ - node firmware DIFFERS from server firmware, checking version...\n");
	printf("[OTA] Node   version: %s\n", cur_desc->version);
	printf("[OTA] Server version: %s\n", new_desc.version);
	if (strncmp(new_desc.version, cur_desc->version, sizeof(new_desc.version)) <= 0)
	{
		printf("[OTA] Server version is NOT newer - skip, no downgrade.\n");
		esp_https_ota_abort(ota_handle);
		return ESP_ERR_NOT_FOUND;
	}

	printf("[OTA] Server version is NEWER -> flashing firmware...\n");
	while (1)
	{
		err = esp_https_ota_perform(ota_handle);
		if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS)
			break;
	}
	if (err == ESP_OK && esp_https_ota_is_complete_data_received(ota_handle))
	{
		err = esp_https_ota_finish(ota_handle);
	}
	else
	{
		esp_https_ota_abort(ota_handle);
		if (err == ESP_OK)
			err = ESP_FAIL;
	}
	if (err != ESP_OK)
		printf("[OTA] esp_https_ota FAILED: %s\n", esp_err_to_name(err));
	return err;
}
