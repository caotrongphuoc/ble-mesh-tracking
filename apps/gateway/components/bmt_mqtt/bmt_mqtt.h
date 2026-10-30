#pragma once

#include <stdbool.h>

#include "mqtt_client.h"

#include "bmt_types.h"

/* Transport-only: higher layers plug in through these handlers (wired in
 * main.c) so bmt_mqtt does not depend on bmt_thingsboard / bmt_ota.
 * Both run in the MQTT event task. */
typedef struct
{
	void (*on_connected)(void);          /* after (re)connect + RPC subscribe */
	void (*on_rpc)(const char* payload); /* RPC request body, null-terminated, max 127 chars */
} bmt_mqtt_handlers_t;

/* Delivers a queued tag report from the worker task. */
typedef void (*bmt_mqtt_tag_report_cb_t)(const bmt_tag_report_t* report, const uint8_t* scanner_mac);

/* Initialise the MQTT client to ThingsBoard and subscribe to the RPC
 * topic. The handlers struct is copied. */
void bmt_mqtt_init(const bmt_mqtt_handlers_t* handlers);

bool bmt_mqtt_is_connected(void);
esp_mqtt_client_handle_t bmt_mqtt_get_client(void);
void bmt_mqtt_enqueue_tag_report(const bmt_tag_report_t* report, const uint8_t* scanner_mac);

/* Called by bmt_thingsboard.c after each successful publish so we can
 * separate mesh_received from mqtt_published (quick debug: is mesh down
 * or is MQTT down?). */
void bmt_mqtt_note_published(void);

void bmt_mqtt_log_stats(void);

/* Task that drains the tag-report queue into on_tag_report
 * (bmt_tb_pub_tag_report in main.c). */
void bmt_mqtt_start_worker(bmt_mqtt_tag_report_cb_t on_tag_report);
