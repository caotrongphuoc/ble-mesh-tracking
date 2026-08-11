#include "bmt_factory_reset.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "BMT_FACTORY_RST";

/* BOOT button (GPIO0) — available on most ESP32 / ESP32-S3 dev boards.
 * Only special at power-on (bootloader download mode); once the app is
 * running, it reads as a plain GPIO input. The EN/RST button will not
 * work for this because holding it keeps the chip in reset, so no code
 * runs to count the hold time. */
#define BMT_FACTORY_RESET_GPIO GPIO_NUM_0
#define BMT_FACTORY_RESET_POLL_MS 100
#define BMT_FACTORY_RESET_HOLD_MS 10000
#define BMT_FACTORY_RESET_TICKS_NEEDED (BMT_FACTORY_RESET_HOLD_MS / BMT_FACTORY_RESET_POLL_MS)

static void do_factory_reset(void)
{
	ESP_LOGW(TAG, "==================================================");
	ESP_LOGW(TAG, "[FACTORY RESET] Held 10s -> erasing all NVS...");
	ESP_LOGW(TAG, "==================================================");
	esp_err_t err = nvs_flash_erase();
	if (err != ESP_OK)
		ESP_LOGE(TAG, "nvs_flash_erase failed: %s", esp_err_to_name(err));
	ESP_LOGW(TAG, "[FACTORY RESET] Done, rebooting...");
	vTaskDelay(pdMS_TO_TICKS(300));
	esp_restart();
}

static void factory_reset_task(void* arg)
{
	(void)arg;
	int held_ticks = 0;

	while (1)
	{
		/* BOOT button pulls GPIO0 to GND while held (active-low). */
		if (gpio_get_level(BMT_FACTORY_RESET_GPIO) == 0)
		{
			held_ticks++;
			if (held_ticks == 1)
			{
				ESP_LOGI(TAG, "[FACTORY RESET] BOOT button held...");
			}
			else if (held_ticks % (1000 / BMT_FACTORY_RESET_POLL_MS) == 0)
			{
				int remain_s = (BMT_FACTORY_RESET_TICKS_NEEDED - held_ticks) * BMT_FACTORY_RESET_POLL_MS / 1000;
				ESP_LOGW(TAG, "[FACTORY RESET] %ds until NVS is erased...", remain_s);
			}

			if (held_ticks >= BMT_FACTORY_RESET_TICKS_NEEDED)
			{
				do_factory_reset();
				/* Never reaches here — esp_restart() already rebooted. */
			}
		}
		else
		{
			/* Released -> reset the counter immediately. Enforces "must be
			 * held CONTINUOUSLY for 10 s" and naturally debounces a single
			 * spurious low read (the counter has to grow through 10 s again). */
			held_ticks = 0;
		}

		vTaskDelay(pdMS_TO_TICKS(BMT_FACTORY_RESET_POLL_MS));
	}
}

void bmt_factory_reset_init(void)
{
	gpio_config_t io_conf = {
	    .pin_bit_mask = (1ULL << BMT_FACTORY_RESET_GPIO),
	    .mode = GPIO_MODE_INPUT,
	    .pull_up_en = GPIO_PULLUP_ENABLE,
	    .pull_down_en = GPIO_PULLDOWN_DISABLE,
	    .intr_type = GPIO_INTR_DISABLE,
	};
	gpio_config(&io_conf);

	xTaskCreate(factory_reset_task, "bmt_factory_rst", 2560, NULL, 3, NULL);
	ESP_LOGI(TAG, "Factory reset watcher started (hold BOOT for 10 s to trigger)");
}
