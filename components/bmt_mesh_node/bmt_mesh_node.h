#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_ble_mesh_defs.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define BMT_PROV_COMPLETE_BIT BIT0

/* Config Server state shared by every node (relay + GATT proxy on). */
#define BMT_MESH_NODE_CFG_SRV_INIT                           \
	{                                                        \
	    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),        \
	    .relay = ESP_BLE_MESH_RELAY_ENABLED,                 \
	    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),    \
	    .beacon = ESP_BLE_MESH_BEACON_ENABLED,               \
	    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_ENABLED,       \
	    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,   \
	    .default_ttl = 7,                                    \
	}

/* Vendor message for the app: every opcode in its op table except
 * BMT_OP_VND_RESET_CMD, which bmt_mesh_node handles itself. Runs in the
 * BLE host task - do not block. */
typedef void (*bmt_mesh_node_vnd_cb_t)(uint32_t opcode, uint16_t src,
                                       const uint8_t* msg, uint16_t len);

/* Register callbacks, init the mesh stack with the app's provisioning
 * data and composition, then either restore provisioning from NVS or
 * enable fresh provisioning. vnd_model is the app's vendor model (used
 * to publish to the Gateway). Callbacks the app needs on top (e.g.
 * health server) must be registered before this call. */
esp_err_t bmt_mesh_node_start(esp_ble_mesh_prov_t* prov, esp_ble_mesh_comp_t* comp,
                              esp_ble_mesh_model_t* vnd_model, bmt_mesh_node_vnd_cb_t on_vnd);

EventGroupHandle_t bmt_mesh_evgrp(void);
bool bmt_mesh_is_provisioned(void);
uint16_t bmt_mesh_node_addr(void);
uint16_t bmt_mesh_net_idx(void);
uint16_t bmt_mesh_app_idx(void);

/* Publish a vendor message to the Gateway (0x0001) with the node's
 * AppKey. ESP_ERR_INVALID_STATE until provisioned + AppKey received. */
esp_err_t bmt_mesh_publish_to_gateway(uint32_t opcode, const void* data, uint16_t len);

/* Report an OTA result back to the Gateway over mesh
 * (0 = success, non-zero = failure). */
esp_err_t bmt_mesh_report_ota_result(uint8_t status);

/* Reset mesh provisioning and reboot once the reset really completed
 * (used by UART 'r' and by RESET_CMD from the Gateway). */
void bmt_mesh_local_reset(void);
