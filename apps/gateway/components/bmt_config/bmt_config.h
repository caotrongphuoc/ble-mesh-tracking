#pragma once
/* User config — edit before building. See docs/00-quickstart.md. */
#define BMT_WIFI_SSID "YOUR_WIFI_SSID"
#define BMT_WIFI_PASS "YOUR_WIFI_PASSWORD"
#define BMT_TB_IP "192.168.1.100" /* Replace with your ThingsBoard host IP on the LAN */
#define BMT_TB_HOST "mqtts://" BMT_TB_IP ":8883"
#define BMT_TB_CN "bmt-tb.local"
#define BMT_TB_GATEWAY_TOKEN "YOUR_GATEWAY_TOKEN"

#define BMT_DEV_NAME_GATEWAY "bmt_gateway"
#define BMT_DEV_NAME_NODE_FMT "bmt_node_%02x%02x%02x%02x%02x%02x"
#define BMT_DEV_NAME_TAG_FMT "bmt_tag_0x%04x"

#define BMT_ROLE_GATEWAY "gateway"
#define BMT_ROLE_RELAY "relay"
#define BMT_ROLE_SCAN "scan"
#define BMT_ROLE_TAG "tag"
#define BMT_PROFILE_TAG "ble_tag"
#define BMT_PROFILE_NODE "ble_mesh_node"
/* Replace 192.168.1.100 with your OTA server IP on the LAN. */
#define BMT_OTA_SERVER_BASE "https://192.168.1.100:8443"
#define BMT_OTA_SCANNER_URL BMT_OTA_SERVER_BASE "/Scanner.bin"
#define BMT_OTA_RELAY_URL BMT_OTA_SERVER_BASE "/Relay.bin"
#define BMT_OTA_GATEWAY_URL BMT_OTA_SERVER_BASE "/Gateway.bin"
#define BMT_OTA_SERVER_CN "bmt-tb.local"
