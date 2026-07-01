<div align="center">
  
![Repo Traffic](https://komarev.com/ghpvc/?username=ble-mesh-tracking&label=Repo+Traffic&color=blue&style=flat-square)

</div>

# BLE Mesh Tracking (BMT)

Indoor zone-level tracking on ESP32. A BLE tag (iPhone iBeacon or ESP32) is picked up by three scan nodes, which forward filtered RSSI over BLE Mesh to a Gateway. The Gateway picks the nearest scanner and pushes the current zone to ThingsBoard over MQTT.

Output is room-presence, not coordinates.

## Data flow

```
Tag (iPhone iBeacon / ESP32)
  | BLE adv
  v
Scan node 0x01 / 0x02 / 0x03
  | BLE Mesh vendor model
  v
Gateway (ESP32-S3 or ESP32 + WiFi)
  | MQTT (ThingsBoard Gateway API)
  v
ThingsBoard (Docker)
```

Only the Gateway has WiFi. Scan nodes reach it via mesh, so a far node can hop through another.

## Zones

| Scanner ID | Zone         |
|------------|--------------|
| 0x01       | bedroom_1    |
| 0x02       | bedroom_2    |
| 0x03       | toilet       |
| 0xFF       | out_of_range |

Rename rooms in `bmt_zone_name()` in the gateway firmware.

## Hardware

- Gateway: ESP32-S3 DevKitC N16R8 (NimBLE, +20 dBm — default) **or** ESP32 WROOM-32 (+9 dBm). TX power auto-selects from `CONFIG_IDF_TARGET_ESP32S3`.
- 3x ESP32 WROOM-32 scan nodes (Bluedroid).
- 1x relay (optional).
- Tag: iPhone with nRF Connect, or ESP32 running `bmt_tag`.
- Docker host for ThingsBoard.

## Repo

```
nodes/
  bmt_gateway/      provisioner + MQTT bridge + zone detection
  bmt_scan_node_1/  ID 0x01 -> bedroom_1   (template — change BMT_SCANNER_ID
  bmt_scan_node_2/  ID 0x02 -> bedroom_2    and UUID byte 15 for others)
  bmt_scan_node_3/  ID 0x03 -> toilet
  bmt_relay/        optional forwarder
  bmt_tag/          optional ESP32 tag
docker-compose.yml
```

ESP-IDF v6.0. Each folder is a standalone project.

## Quick start

**1. ThingsBoard**

```bash
docker compose up -d
```

Open `http://localhost:8080` (`tenant@thingsboard.org` / `tenant`). Create device `bmt_gateway`, enable "Is gateway", copy the access token. Sub-devices (`bmt_node_0xXXXX`, `bmt_tag_0xYYYY`) auto-provision.

**2. Configure Gateway** — top of `nodes/bmt_gateway/main/main.c`:

```c
#define BMT_WIFI_SSID          "YourWiFi"
#define BMT_WIFI_PASS          "YourPassword"
#define BMT_TB_HOST            "mqtt://<PC_IP>:1883"   // LAN IP, not localhost
#define BMT_TB_GATEWAY_TOKEN   "<paste_token_here>"
```

**3. Flash** — Gateway first, then scan nodes one at a time. Wait for `=== Scan node 0x000X READY ===` between each.

```bash
cd nodes/bmt_gateway && idf.py set-target esp32s3   # only if using S3
idf.py -p COMx flash monitor
```

**4. Test** — advertise an iBeacon (UUID `AB000000...`, Major `0x0001` PERSON / `0x0002` ASSET, Minor = tag ID). Walk between rooms, watch the `zone` field on `bmt_tag_0xXXXX`.

## How it works

**Time-division radio.** One BLE radio per ESP32, so each scan node alternates: 1200 ms GAP scan, 300 ms mesh publish (1.5 s cycle). Scanners are phase-offset by ID (0 / 500 / 1000 ms) to stay out of each other's publish window.

**RSSI.** 1-D Kalman filter per tag. Distance is a log-distance estimate (`n = 2.5`), reported but not used in the zone decision.

**Zone detection.** Latest RSSI per scanner per tag, valid for 3.5 s. Zone = strongest scanner. To stop boundary flicker, a new zone must beat the current by ≥ 5 dBm (`BMT_ZONE_HYSTERESIS_DBM`). Tag goes `out_of_range` after 10 s of silence.

**Vendor model** — CID `0x02E5`, model `0x0000`. Payloads kept to 8 bytes so each fits one unsegmented PDU (no ACK, no retransmit stalls):

```c
// opcode 0x00 — tag report (every cycle)
struct { uint8_t scanner_id, tag_type; uint16_t tag_id;
         int8_t rssi; int16_t distance_dm; uint8_t loss_pct; };

// opcode 0x02 — node health (every 30 s)
struct { uint8_t scanner_id; int8_t chip_temp_c;
         uint16_t vdd_mv, free_heap_kb, uptime_min; };
```

**Gateway bridge.** Mesh callback only enqueues; a worker task drains the queue to MQTT. A slow or dropped connection can't block the mesh task or starve one scanner behind another.

## Serial commands

Gateway: `1` list nodes · `2` list tracked tags · `0` wipe NVS and reboot.
Scan node / relay: `1` print status · `r` reset provisioning.

Clean reprovision: `0` on Gateway, then `r` on each node, then reconnect nodes one by one waiting for `=== READY ===`.

## Common issues

- **MQTT won't connect** — Docker down, wrong IP in `BMT_TB_HOST`, or firewall on 1883. Use LAN IP, not `localhost`.
- **Only one scan node shows up** — all three flashed from the same folder. Flash each from its own.
- **Far scanner never reaches Gateway** — single-hop BLE Mesh is ~10 m indoors and much less through walls. Move the Gateway central, or add the relay.
- **Tag flickers between zones** — raise `BMT_ZONE_HYSTERESIS_DBM` to 8–10.
- **`esp_read_mac` undeclared** — add `#include "esp_mac.h"`. ESP-IDF v6.0 dropped the implicit include.

## Stack

ESP-IDF v6.0 · BLE Mesh vendor model · NimBLE (Gateway) / Bluedroid (nodes) · Kalman RSSI · log-distance path loss · MQTT 3.1.1 · ThingsBoard CE 3.7.0.

## Status

Done: provisioning, vendor model, three scan nodes with time-division radio, zone detection with hysteresis, node health telemetry, auto-provisioned sub-devices.
Planned: dashboard (floor plan + timeline), trilateration.

## Contact

Cao Trong Phuoc — Embedded / Software Engineer
GitHub: https://github.com/caotrongphuoc · LinkedIn: https://www.linkedin.com/in/cao-trong-phuoc/
