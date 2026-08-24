#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
void bmt_mesh_generate_keys_if_needed(void);

/* Initialise the mesh: register callbacks, add NetKey/AppKey if not
 * already present, bind the AppKey to the local vendor model. Call
 * after bluetooth_init(). */
esp_err_t bmt_mesh_init(void);

void bmt_mesh_start_node_ping(void);

/* Publish a vendor message to dst (unicast or 0xFFFF broadcast). Shared
 * by OTA_TRIGGER (bmt_ota.c) and RESET_CMD (bmt_watchdog.c). */
esp_err_t bmt_mesh_publish(uint16_t dst, uint32_t opcode, const void* data, uint16_t len);
uint32_t bmt_mesh_get_received_count(void);

/* Wipe every provisioned node from the mesh stack (provisioner node
 * table). Used by UART '9' FULL RESET and by bmt_watchdog.c. Returns
 * the number of nodes wiped. */
int bmt_mesh_wipe_all_provisioned(void);

/* Print NetKey / AppKey as hex on UART at boot (banner). The keys stay
 * private inside this module - not exposed via pointers or arrays. */
void bmt_mesh_print_keys(void);
