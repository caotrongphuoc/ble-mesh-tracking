#include "bmt_uart.h"

#include <inttypes.h>
#include <stdio.h>

#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bmt_types.h"
#include "bmt_mesh.h"
#include "bmt_ota.h"
#include "bmt_scan_core.h"

static void print_version_uptime(void)
{
	const esp_app_desc_t* desc = esp_app_get_description();
	uint32_t uptime_s = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
	printf("Version    : %s (build %s %s)\n", desc->version, desc->date, desc->time);
	printf("Uptime     : %02" PRIu32 "h %02" PRIu32 "m %02" PRIu32 "s\n",
	       uptime_s / 3600, (uptime_s % 3600) / 60, uptime_s % 60);
}

static void uart_cmd_task(void* arg)
{
	(void)arg;
	uart_config_t cfg = {
	    .baud_rate = 115200,
	    .data_bits = UART_DATA_8_BITS,
	    .parity = UART_PARITY_DISABLE,
	    .stop_bits = UART_STOP_BITS_1,
	    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
	    .source_clk = UART_SCLK_DEFAULT,
	};
	uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
	uart_param_config(UART_NUM_0, &cfg);

	printf("\n===== BMT SCAN NODE (modular) =====\n");
	printf("Scanner ID : 0x%02X\n", bmt_scan_core_scanner_id());
	printf("Node type  : 0x%02X (scanner)\n", BMT_NODE_TYPE);
	/* Print the MAC at boot — needed to know which board this is when
	 * filling in ZONE_MAP on ThingsBoard (which MAC maps to room_1/2/3). */
	{
		const uint8_t* uuid = bmt_mesh_uuid();
		printf("MAC        : ");
		for (int i = 4; i < 10; i++)
			printf("%02x", uuid[i]);
		printf("\n");
	}
	printf("UUID       : SCAN + MAC (auto)\n");
	printf("Commands:\n");
	printf("  r -> RESET mesh provision\n");
	printf("  i -> SET scanner ID (1-8)\n");
	printf("  o -> TEST WiFi OTA\n");
	printf("  1 -> STATUS\n");
	printf("====================================\n");

	uint8_t ch;
	while (1)
	{
		int len = uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(200));
		if (len <= 0 || ch == '\r' || ch == '\n')
			continue;

		switch (ch)
		{
		case 'r':
		case 'R':
			printf("\n[UART] Reset mesh — will reboot when done...\n");
			bmt_mesh_local_reset();
			break;

		case 'i':
		case 'I':
			printf("\n[UART] Enter scanner ID (1-8): ");
			fflush(stdout);
			{
				uint8_t id_char = 0;
				int r;
				do
				{
					r = uart_read_bytes(UART_NUM_0, &id_char, 1, pdMS_TO_TICKS(10000));
				} while (r > 0 && (id_char == '\r' || id_char == '\n'));
				uint8_t new_id = id_char - '0';
				if (r > 0 && new_id >= 1 && new_id <= 8)
				{
					bmt_scan_core_set_scanner_id(new_id);
					printf("%d\n[UART] Scanner ID set to 0x%02X\n"
					       "[UART] Press 'r' to reset and apply the new UUID\n",
					       new_id, new_id);
				}
				else
				{
					printf("\n[UART] Invalid ID — must be 1 to 8\n");
				}
			}
			break;

		case '1':
			printf("\n===== SCANNER STATUS =====\n");
			printf("Scanner ID : 0x%02X\n", bmt_scan_core_scanner_id());
			printf("MAC        : ");
			{
				const uint8_t* uuid = bmt_mesh_uuid();
				for (int i = 4; i < 10; i++)
					printf("%02x", uuid[i]);
				printf("\n");
			}
			printf("UUID       : ");
			{
				const uint8_t* uuid = bmt_mesh_uuid();
				for (int i = 0; i < 16; i++)
					printf("%02X%s", uuid[i], i < 15 ? ":" : "\n");
			}
			printf("Node addr  : 0x%04X\n", bmt_mesh_node_addr());
			printf("App idx    : 0x%04X\n", bmt_mesh_app_idx());
			printf("OTA state  : %s\n", bmt_ota_is_triggered() ? "WiFi OTA running" : "IDLE");
			printf("Heap free  : %lu bytes\n", (unsigned long)esp_get_free_heap_size());
			print_version_uptime();
			printf("==========================\n");
			break;

		case 'o':
		case 'O':
			if (bmt_ota_is_triggered())
			{
				printf("\n[OTA] Already running!\n");
				break;
			}
			printf("\n[UART] Manual OTA trigger — starting WiFi OTA...\n");
			bmt_ota_trigger();
			break;

		default:
			printf("[UART] Unknown: %c  (r=reset, i=set ID, o=OTA, 1=status)\n", ch);
			break;
		}
	}
}

esp_err_t bmt_uart_init(void)
{
	xTaskCreate(uart_cmd_task, "bmt_uart", 2048, NULL, 3, NULL);
	return ESP_OK;
}
