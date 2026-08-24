#include <assert.h>
#include "bmt_uart.h"

#include <stdio.h>

#include "driver/uart.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bmt_config.h"
#include "bmt_mesh.h"
#include "bmt_mqtt.h"
#include "bmt_node_table.h"
#include "bmt_ota.h"
#include "bmt_scan_list.h"
#include "bmt_thingsboard.h"
#include "bmt_types.h"
#include "bmt_zone.h"

#define BMT_UART_NUM UART_NUM_0
#define BMT_UART_BAUD 115200
#define BMT_UART_RX_BUF_SIZE 2048

static void print_status(void)
{
	printf("\n=========== GATEWAY COMMANDS ===========\n");
	printf("1 -> LIST PROVISIONED NODES\n");
	printf("2 -> LIST TRACKED TAGS + ZONES\n");
	printf("3 -> MQTT / MESH STATS\n");
	printf("s -> SCAN BEACONS (10s)\n");
	printf("p -> PROVISION SCAN LIST\n");
	printf("a -> AUTO PROVISION MODE\n");
	printf("m -> MANUAL PROVISION MODE\n");
	printf("4 -> SHOW STATUS\n");
	printf("u -> [OTA] UPDATE SCANNER/RELAY VIA MESH\n");
	printf("g -> [OTA] UPDATE GATEWAY FIRMWARE\n");
	printf("0 -> SOFT RESET (restart, keeps mesh + NVS intact)\n");
	printf("9 -> FULL RESET (erase everything, re-provision)\n");
	printf("Provision mode: %s\n",
	       bmt_scan_list_get_mode() == BMT_PROV_MODE_AUTO ? "AUTO" : "MANUAL");
	printf("=========================================\n");
}

static void uart_cmd_task(void* arg)
{
	uint8_t ch;
	(void)arg;
	print_status();

	while (1)
	{
		int len = uart_read_bytes(BMT_UART_NUM, &ch, 1, pdMS_TO_TICKS(200));
		if (len <= 0 || ch == '\r' || ch == '\n')
			continue;

		switch (ch)
		{
		case '1':
			bmt_node_table_print();
			break;
		case '2':
			bmt_zone_log_tracked();
			break;
		case '3':
			bmt_mqtt_log_stats();
			break;

		case 'g':
			printf("\n[OTA] Updating GATEWAY: %s\n", BMT_OTA_GATEWAY_URL);
			bmt_ota_gateway_self_update();
			break;

		case 's':
			printf("\n[UART] Starting MANUAL SCAN...\n");
			bmt_scan_list_start();
			break;
		case 'p':
			if (bmt_scan_list_get_mode() != BMT_PROV_MODE_MANUAL)
				printf("\n[UART] Not in MANUAL mode. Press m first.\n");
			else
				bmt_scan_list_provision();
			break;
		case 'a':
			bmt_scan_list_set_mode(BMT_PROV_MODE_AUTO);
			printf("\n[UART] AUTO provision mode\n");
			break;
		case 'm':
			bmt_scan_list_set_mode(BMT_PROV_MODE_MANUAL);
			printf("\n[UART] MANUAL provision mode\n");
			break;
		case '4':
			print_status();
			break;

		case 'u':
			if (bmt_ota_is_running())
			{
				printf("\n[OTA] Already running!\n");
				break;
			}
			printf("\n[OTA] Select target:\n      s = Scanner\n      r = Relay\n      (5s...)\n");
			{
				uint8_t sel = 0;
				int r;
				do
				{
					r = uart_read_bytes(BMT_UART_NUM, &sel, 1, pdMS_TO_TICKS(5000));
				} while (r > 0 && (sel == '\r' || sel == '\n'));
				if (sel == 'r' || sel == 'R')
				{
					printf("[OTA] Starting OTA for RELAY...\n");
					bmt_ota_trigger_all_relays();
				}
				else
				{
					printf("[OTA] Starting OTA for SCANNER...\n");
					bmt_ota_trigger_all_scanners();
				}
			}
			break;

		case '0':
			printf("\n[UART] SOFT RESET - restart, keeping the whole mesh NVS...\n");
			bmt_zone_reset_all();
			vTaskDelay(pdMS_TO_TICKS(300));
			esp_restart();
			break;

		case '9':
			printf("\n[UART] FULL RESET - erase everything, re-provision after reboot...\n");
			{
				int erased = bmt_mesh_wipe_all_provisioned();
				printf("[UART] Erased %d node(s)\n", erased);
			}
			bmt_node_table_clear();
			bmt_zone_reset_all();
			vTaskDelay(pdMS_TO_TICKS(500));
			esp_restart();
			break;

		default:
			printf("\n[UART] Unknown: %c\n", ch);
			print_status();
			break;
		}
	}
}

void bmt_uart_start(void)
{
	uart_driver_delete(BMT_UART_NUM);
	const uart_config_t cfg = {
	    .baud_rate = BMT_UART_BAUD,
	    .data_bits = UART_DATA_8_BITS,
	    .parity = UART_PARITY_DISABLE,
	    .stop_bits = UART_STOP_BITS_1,
	    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
	    .source_clk = UART_SCLK_DEFAULT,
	};
	ESP_ERROR_CHECK(uart_driver_install(BMT_UART_NUM, BMT_UART_RX_BUF_SIZE, 0, 0, NULL, 0));
	ESP_ERROR_CHECK(uart_param_config(BMT_UART_NUM, &cfg));
	assert(xTaskCreate(uart_cmd_task, "bmt_uart", 6144, NULL, 4, NULL) == pdPASS);
}
