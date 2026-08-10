#include "bmt_uart.h"

#include <stdio.h>
#include <inttypes.h>

#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bmt_config.h"
#include "bmt_mesh.h"

static void print_version(void)
{
	const esp_app_desc_t* desc = esp_app_get_description();
	printf("Version   : %s (build %s %s)\n", desc->version, desc->date, desc->time);
}

/* Periodically print a health log to UART — purely for on-site debug
 * cable monitoring. Has no effect on the Relay's forward / OTA /
 * reset logic. */
#define BMT_RELAY_MONITOR_INTERVAL_MS 30000
static void print_relay_mac(void)
{
	uint8_t mac[6];
	esp_read_mac(mac, ESP_MAC_BT);
	printf("MAC       : %02x%02x%02x%02x%02x%02x\n",
	       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void uart_cmd_task(void* arg)
{
	(void)arg;
	const uart_config_t cfg = {
	    .baud_rate = 115200,
	    .data_bits = UART_DATA_8_BITS,
	    .parity = UART_PARITY_DISABLE,
	    .stop_bits = UART_STOP_BITS_1,
	    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
	    .source_clk = UART_SCLK_DEFAULT,
	};
	uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
	uart_param_config(UART_NUM_0, &cfg);

	printf("\n===== BMT RELAY (modular) COMMANDS =====\n");
	printf("r -> RESET mesh (erase NVS, back to unprovisioned)\n");
	printf("1 -> STATUS (current)\n");
	printf("=========================================\n");

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
			/* bmt_mesh_local_reset() now reboots exactly when the mesh
			 * reset REALLY completes (via event); no more fixed delay here. */
			printf("\n[UART] Resetting mesh — will reboot when done...\n");
			bmt_mesh_local_reset();
			break;

		case '1':
			printf("\n===== RELAY STATUS =====\n");
			print_relay_mac();
			printf("UUID      : RELAY...:%02X\n", BMT_RELAY_UUID[15]);
			printf("Node addr : 0x%04X (%s)\n", bmt_mesh_node_addr(),
			       bmt_mesh_node_addr() != 0x0000 ? "PROVISIONED" : "UNPROVISIONED");
			printf("Net idx   : 0x%04X\n", bmt_mesh_net_idx());
			printf("App idx   : 0x%04X\n", bmt_mesh_app_idx());
			printf("Relay     : %s\n", bmt_mesh_relay_enabled() ? "ENABLED" : "DISABLED");
			{
				uint32_t uptime_s = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
				printf("Uptime    : %02" PRIu32 "h %02" PRIu32 "m %02" PRIu32 "s\n",
				       uptime_s / 3600, (uptime_s % 3600) / 60, uptime_s % 60);
			}
			print_version();
			printf("========================\n");
			break;

		default:
			printf("[UART] Unknown: %c  (r=reset, 1=status)\n", ch);
			break;
		}
	}
}

/* Wait until provisioning is complete, then start counting uptime
 * and print a health log every BMT_RELAY_MONITOR_INTERVAL_MS — for
 * on-site UART monitoring only. */
static void relay_monitor_task(void* arg)
{
	(void)arg;

	xEventGroupWaitBits(bmt_mesh_evgrp(), BMT_PROV_COMPLETE_BIT,
	                    pdFALSE, pdTRUE, portMAX_DELAY);
	vTaskDelay(pdMS_TO_TICKS(5000));

	printf("\n========== RELAY READY ==========\n");
	print_relay_mac();
	printf("UUID      : RELAY...:%02X\n", BMT_RELAY_UUID[15]);
	printf("Node addr : 0x%04X\n", bmt_mesh_node_addr());
	printf("Relay     : %s\n", bmt_mesh_relay_enabled() ? "ENABLED" : "DISABLED");
	printf("RESET_CMD : supported (remote watchdog)\n");
	printf("==================================\n");

	while (1)
	{
		vTaskDelay(pdMS_TO_TICKS(BMT_RELAY_MONITOR_INTERVAL_MS));

		uint32_t uptime_s = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
		uint32_t h = uptime_s / 3600;
		uint32_t m = (uptime_s % 3600) / 60;
		uint32_t s = uptime_s % 60;

		printf("\n===== RELAY HEALTH =====\n");
		printf("Status    : %s\n",
		       bmt_mesh_node_addr() != 0x0000 ? "PROVISIONED & RELAYING" : "UNPROVISIONED");
		printf("Node addr : 0x%04X\n", bmt_mesh_node_addr());
		printf("Uptime    : %02" PRIu32 "h %02" PRIu32 "m %02" PRIu32 "s\n", h, m, s);
		printf("Free heap : %lu bytes\n", (unsigned long)esp_get_free_heap_size());
		print_version();
		printf("========================\n");
	}
}

esp_err_t bmt_uart_init(void)
{
	xTaskCreate(uart_cmd_task, "bmt_uart", 2048, NULL, 3, NULL);
	xTaskCreate(relay_monitor_task, "bmt_relay_mon", 2048, NULL, 3, NULL);
	return ESP_OK;
}
