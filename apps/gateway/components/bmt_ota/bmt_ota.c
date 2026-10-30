#include <assert.h>
#include "bmt_ota.h"
#include "bmt_ota_key.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "host/ble_hs.h"

#include "bmt_config.h"
#include "bmt_https_ota.h"
#include "bmt_mesh.h"
#include "bmt_node_table.h"
#include "bmt_thingsboard.h"
#include "bmt_types.h"

static const char* TAG = "BMT_OTA";

#define BMT_OTA_HTTP_TIMEOUT_MS 15000
#define BMT_OTA_NODE_GAP_MS 90000        /* 90 s per node: enough to download ~840 KB and reboot */
#define BMT_OTA_BEACON_DURATION_MS 15000 /* 15 s: enough GAP scan cycles for the scanner to catch the beacon */

static atomic_bool s_running = false;

static void self_update_task(void* arg)
{
	(void)arg;
	printf("\n[OTA] ===== Gateway self-update =====\n");
	printf("[OTA] URL: %s\n", BMT_OTA_GATEWAY_URL);

	bmt_https_ota_cfg_t fetch = {
	    .url = BMT_OTA_GATEWAY_URL,
	    .server_cn = BMT_OTA_SERVER_CN,
	    .timeout_ms = BMT_OTA_HTTP_TIMEOUT_MS,
	};
	esp_err_t err = bmt_https_ota_fetch_and_flash(&fetch);
	if (err == ESP_ERR_NOT_FOUND)
	{
		atomic_store(&s_running, false);
		vTaskDelete(NULL);
		return;
	}
	if (err == ESP_OK)
	{
		printf("[OTA] ===== OTA SUCCESS - rebooting =====\n");
		bmt_tb_pub_gateway_ota_result(true);
		vTaskDelay(pdMS_TO_TICKS(1000));
		esp_restart();
	}

	bmt_tb_pub_gateway_ota_result(false);
	printf("[OTA] Gateway self-update FAILED: %s\n", esp_err_to_name(err));
	atomic_store(&s_running, false);
	vTaskDelete(NULL);
}

esp_err_t bmt_ota_gateway_self_update(void)
{
	bool expected = false;
	if (!atomic_compare_exchange_strong(&s_running, &expected, true))
	{
		ESP_LOGW(TAG, "OTA already running");
		return ESP_ERR_INVALID_STATE;
	}
	if (xTaskCreate(self_update_task, "bmt_ota_self", 8192, NULL, 4, NULL) != pdPASS)
	{
		atomic_store(&s_running, false);
		return ESP_ERR_NO_MEM;
	}
	return ESP_OK;
}

/* Beacon format: Flags + Manufacturer Specific (CID Espressif + "BMT" + 0xFA + target + mac16) */
static uint8_t s_beacon_raw[14] = {
    0x02, 0x01, 0x06, /* Flags: LE General Discoverable */
    0x0A, 0xFF,       /* Manufacturer Specific, length=10 */
    0xE5, 0x02,       /* CID Espressif (little-endian) */
    'B', 'M', 'T',    /* Magic "BMT" */
    0xFA,             /* Command: OTA trigger */
    0x01,             /* target_type - filled in before send */
    0x00, 0x00,       /* mac16 - HMAC-16 filled in before send */
};

static int adv_gap_event(struct ble_gap_event* event, void* arg)
{
	(void)arg;
	if (event->type == BLE_GAP_EVENT_ADV_COMPLETE)
		ESP_LOGI(TAG, "[OTA] beacon adv complete, reason=%d", event->adv_complete.reason);
	return 0;
}

static esp_err_t beacon_send(uint8_t target_type)
{
	s_beacon_raw[11] = target_type;
	uint16_t mac = bmt_ota_key_hmac16(&s_beacon_raw[7], 5); /* magic(3)+cmd(1)+target(1) */
	s_beacon_raw[12] = (uint8_t)(mac >> 8);
	s_beacon_raw[13] = (uint8_t)(mac & 0xFF);

	int rc = ble_gap_adv_set_data(s_beacon_raw, sizeof(s_beacon_raw));
	if (rc != 0)
	{
		ESP_LOGE(TAG, "[OTA] ble_gap_adv_set_data FAILED rc=%d", rc);
		return ESP_FAIL;
	}

	struct ble_gap_adv_params adv_params = {0};
	adv_params.conn_mode = BLE_GAP_CONN_MODE_NON;
	adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
	adv_params.itvl_min = 32; /* 20ms = 32 * 0.625ms */
	adv_params.itvl_max = 64; /* 40ms */

	rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, adv_gap_event, NULL);
	if (rc != 0)
	{
		ESP_LOGE(TAG, "[OTA] ble_gap_adv_start FAILED rc=%d - the mesh host may already be advertising, "
		              "falling back to mesh unicast",
		         rc);
		return ESP_FAIL;
	}

	printf("[OTA] NimBLE beacon broadcasting (target=0x%02x, mac16=0x%04x, %ds)...\n",
	       target_type, mac, BMT_OTA_BEACON_DURATION_MS / 1000);
	vTaskDelay(pdMS_TO_TICKS(BMT_OTA_BEACON_DURATION_MS));
	ble_gap_adv_stop();
	printf("[OTA] NimBLE beacon stopped\n");
	return ESP_OK;
}
typedef struct
{
	uint8_t node_type;
	bool is_scan_filter;
} distribute_arg_t;

static void distribute_task(void* arg)
{
	distribute_arg_t d = *(distribute_arg_t*)arg;
	uint8_t target_type = d.node_type;
	bool is_scan = d.is_scan_filter;
	const char* type_str = is_scan ? "SCANNER" : "RELAY";

	int node_count = 0;
	for (int i = 0; i < bmt_node_table_capacity(); i++)
	{
		const bmt_node_t* n = bmt_node_table_get(i);
		if (!n || !n->used || !n->config_done)
			continue;
		if (is_scan && !n->is_scan)
			continue;
		if (!is_scan && !n->is_relay)
			continue;
		node_count++;
	}
	if (node_count == 0)
	{
		printf("[OTA] No configured %s nodes found\n", type_str);
		atomic_store(&s_running, false);
		vTaskDelete(NULL);
		return;
	}
	printf("[OTA] Found %d %s node(s)\n", node_count, type_str);

	if (is_scan)
	{
		printf("[OTA] Broadcasting NimBLE beacon (%ds) - all %d scanners OTA simultaneously\n",
		       BMT_OTA_BEACON_DURATION_MS / 1000, node_count);
		if (beacon_send(target_type) == ESP_OK)
		{
			printf("[OTA] Waiting %ds for all scanners to download + reboot...\n", BMT_OTA_NODE_GAP_MS / 1000);
			vTaskDelay(pdMS_TO_TICKS(BMT_OTA_NODE_GAP_MS));
			printf("[OTA] ===== Scanner OTA complete =====\n");
			atomic_store(&s_running, false);
			vTaskDelete(NULL);
			return;
		}
		printf("[OTA] Beacon broadcast failed - falling back to mesh unicast (slower but reliable)\n");
	}

	int idx = 0;
	for (int i = 0; i < bmt_node_table_capacity(); i++)
	{
		bmt_node_t* n = bmt_node_table_get(i);
		if (!n || !n->used || !n->config_done)
			continue;
		if (is_scan && !n->is_scan)
			continue;
		if (!is_scan && !n->is_relay)
			continue;

		idx++;
		printf("\n[OTA] -- %s %d/%d: 0x%04x (%s) --\n", type_str, idx, node_count, n->addr, n->name);

		for (int retry = 0; retry < 5; retry++)
		{
			esp_err_t e = bmt_mesh_publish(n->addr, BMT_OP_VND_OTA_TRIGGER, &target_type, sizeof(target_type));
			printf("[OTA] TRIGGER -> 0x%04x [%d/5]: %s\n", n->addr, retry + 1,
			       e == ESP_OK ? "sent" : esp_err_to_name(e));
			vTaskDelay(pdMS_TO_TICKS(500));
		}
		printf("[OTA] %s 0x%04x triggered - waiting %ds...\n", type_str, n->addr, BMT_OTA_NODE_GAP_MS / 1000);
		vTaskDelay(pdMS_TO_TICKS(BMT_OTA_NODE_GAP_MS));
	}

	printf("\n[OTA] ===== All %s nodes triggered! =====\n", type_str);
	atomic_store(&s_running, false);
	vTaskDelete(NULL);
}

static esp_err_t start_distribute(bool is_scan_filter, uint8_t node_type)
{
	bool expected = false;
	if (!atomic_compare_exchange_strong(&s_running, &expected, true))
	{
		ESP_LOGW(TAG, "OTA already running");
		return ESP_ERR_INVALID_STATE;
	}
	static distribute_arg_t s_arg; /* static: outlives the caller, the task reads it once at start */
	s_arg.is_scan_filter = is_scan_filter;
	s_arg.node_type = node_type;
	if (xTaskCreate(distribute_task, "bmt_ota_dst", 4096, &s_arg, 4, NULL) != pdPASS)
	{
		atomic_store(&s_running, false);
		return ESP_ERR_NO_MEM;
	}
	return ESP_OK;
}

esp_err_t bmt_ota_trigger_all_scanners(void)
{
	return start_distribute(true, BMT_NODE_TYPE_SCANNER);
}
esp_err_t bmt_ota_trigger_all_relays(void)
{
	return start_distribute(false, BMT_NODE_TYPE_RELAY);
}
bool bmt_ota_is_running(void)
{
	return atomic_load(&s_running);
}

#define BMT_OTA_GATEWAY_AUTO_CHECK_INTERVAL_MS (3 * 60 * 1000)

static void gateway_auto_check_task(void* arg)
{
	(void)arg;
	while (1)
	{
		vTaskDelay(pdMS_TO_TICKS(BMT_OTA_GATEWAY_AUTO_CHECK_INTERVAL_MS));
		bmt_ota_gateway_self_update();
	}
}

void bmt_ota_start_auto_check(void)
{
	assert(xTaskCreate(gateway_auto_check_task, "bmt_ota_chk", 3072, NULL, 3, NULL) == pdPASS);
}
