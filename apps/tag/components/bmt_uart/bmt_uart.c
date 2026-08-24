#include <assert.h>
#include "bmt_uart.h"

#include <stdio.h>

#include "driver/uart.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bmt_config.h"
#include "bmt_beacon.h"

static void uart_task(void* arg)
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

	printf("\n===== TAG COMMANDS: 1=status =====\n");

	uint8_t ch;
	while (1)
	{
		int len = uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(200));
		if (len <= 0 || ch == '\r' || ch == '\n')
			continue;

		if (ch == '1')
		{
			printf("\n========== TAG STATUS ==========\n");
			printf("UUID      : %02X%02X%02X%02X-%02X%02X-%02X%02X-"
			       "%02X%02X-%02X%02X%02X%02X%02X%02X\n",
			       BMT_SYSTEM_UUID[0], BMT_SYSTEM_UUID[1],
			       BMT_SYSTEM_UUID[2], BMT_SYSTEM_UUID[3],
			       BMT_SYSTEM_UUID[4], BMT_SYSTEM_UUID[5],
			       BMT_SYSTEM_UUID[6], BMT_SYSTEM_UUID[7],
			       BMT_SYSTEM_UUID[8], BMT_SYSTEM_UUID[9],
			       BMT_SYSTEM_UUID[10], BMT_SYSTEM_UUID[11],
			       BMT_SYSTEM_UUID[12], BMT_SYSTEM_UUID[13],
			       BMT_SYSTEM_UUID[14], BMT_SYSTEM_UUID[15]);
			printf("Major     : 0x%04X (%s)\n", BMT_TAG_MAJOR,
			       BMT_TAG_MAJOR == 0x0001 ? "PERSON" : "ASSET");
			printf("Minor     : 0x%04X  (Tag ID = %u)\n",
			       BMT_TAG_MINOR, BMT_TAG_MINOR);
			printf("TX Power  : %d dBm  (measured power ref - calibrate!)\n",
			       BMT_TAG_TX_POWER);
			printf("Sequence  : %u\n", bmt_beacon_sequence());
			printf("MAC-16    : 0x%04X\n", bmt_beacon_last_mac16());
			printf("ADV state : %s\n", bmt_beacon_is_active() ? "ACTIVE" : "STOPPED");
			printf("Heap free : %lu bytes\n",
			       (unsigned long)esp_get_free_heap_size());
			printf("================================\n");
		}
	}
}

esp_err_t bmt_uart_init(void)
{
	assert(xTaskCreate(uart_task, "uart", 2048, NULL, 3, NULL) == pdPASS);
	return ESP_OK;
}
