# BLE Mesh Tracking

Indoor positioning system on ESP32. Three scan nodes detect a BLE tag (iPhone iBeacon or custom ESP32), forward RSSI data through BLE Mesh to a Gateway, which decides the tag's current zone (nearest-scanner) and pushes it to a ThingsBoard dashboard over MQTT.

---

## Demo

> Live dashboard, hardware setup, and walk-through video.

---

## Architecture

```
   Tag (iPhone / ESP32)
          │ BLE advertising
          ▼
   Scan Node 1 / 2 / 3 ──── BLE Mesh (Vendor Model)
          │
          ▼
   Gateway (ESP32 + WiFi) ──── MQTT ──── ThingsBoard (Docker)
                                              │
                                              ▼
                                         Dashboard
```

Each scan node samples RSSI, applies Kalman filtering, estimates distance, and publishes `{scanner_id, tag_id, rssi, distance}` to the Gateway. The Gateway runs nearest-scanner zone detection with 5 dBm hysteresis and publishes the result to ThingsBoard via the MQTT Gateway API.

---

## Hardware

- 1 × ESP32 DevKitC WROOM-32 — Gateway
- 3 × ESP32 DevKitC WROOM-32 — Scan nodes
- 1 × ESP32 DevKitC WROOM-32 — Relay (optional, extends mesh range)
- iPhone with nRF Connect (or another ESP32 running `bmt_tag` firmware) — Tag
- A machine running Docker (Windows / Linux / macOS) — ThingsBoard host

---

## Repo structure

```
bmt_gateway/         Gateway firmware: provisioner + MQTT bridge + zone detection
bmt_scan_node_1/     Scan node, ID=0x01 → zone "room_1"
bmt_scan_node_2/     Scan node, ID=0x02 → zone "room_2"
bmt_scan_node_3/     Scan node, ID=0x03 → zone "room_3"
bmt_relay/           Relay firmware (passive forwarder, optional)
bmt_tag/             ESP32 tag firmware (optional)
docker-compose.yml   ThingsBoard self-host stack
```

Each firmware is a standalone ESP-IDF v6.0 project. Scan nodes are split into three folders because each has a different UUID and Scanner ID hardcoded in source.

---

## Quick start

**1. Start ThingsBoard**
```bash
docker compose up -d
```
Open `http://localhost:8080` (login: `tenant@thingsboard.org` / `tenant`). Create a device named `bmt_gateway`, enable the "Is gateway" toggle, copy the access token.

**2. Configure Gateway firmware** — edit `bmt_gateway/main/main.c`:
```c
#define BMT_WIFI_SSID            "YourWiFi"
#define BMT_WIFI_PASS            "YourPassword"
#define BMT_TB_HOST              "mqtt://<PC_IP>:1883"
#define BMT_TB_GATEWAY_TOKEN     "<paste_token_here>"
```

**3. Flash all boards** (Gateway first, then scan nodes one at a time):
```bash
cd bmt_gateway       && idf.py -p COMx flash monitor
cd bmt_scan_node_1   && idf.py -p COMx flash monitor
cd bmt_scan_node_2   && idf.py -p COMx flash monitor
cd bmt_scan_node_3   && idf.py -p COMx flash monitor
```
Wait for `=== Scan node 0x000X READY ===` on the Gateway before flashing the next.

**4. Test** — open nRF Connect on iPhone, advertise an iBeacon with Major `0x0001` (PERSON) or `0x0002` (ASSET), walk through the rooms, watch the `zone` field update on the ThingsBoard device `bmt_tag_0xXXXX`.

---

## How it works

**Zone detection.** Each scanner has an ID (0x01–0x03) mapped to a room name. The Gateway tracks the latest RSSI from each scanner per tag (window: 3.5 s). The zone is the room of the scanner with the strongest RSSI. To avoid flickering, a new zone is only accepted if its RSSI exceeds the current one by at least 5 dBm.

**Time-division radio.** An ESP32 has a single BLE radio, so scan nodes alternate between GAP scanning (1000 ms) and mesh publishing (500 ms). Each tag is reported once per 1.5 s per scanner.

**Vendor Model payload.** 8 bytes — fits in a single unsegmented BLE Mesh PDU, no ACK needed:
```c
struct {
    uint8_t  scanner_id;
    uint8_t  tag_type;      // 0x01=PERSON, 0x02=ASSET
    uint16_t tag_id;
    int8_t   rssi;          // filtered (Kalman)
    int16_t  distance_dm;   // decimeters
    uint8_t  loss_pct;
};
```

**ThingsBoard devices.** The Gateway uses the [MQTT Gateway API](https://thingsboard.io/docs/reference/gateway-mqtt-api/) — sub-devices (`bmt_node_0xXXXX`, `bmt_tag_0xYYYY`) are auto-provisioned, no manual setup needed.

---

## Serial commands

Gateway:
- `1` — list provisioned nodes
- `2` — list tracked tags with current zone and per-scanner RSSI
- `0` — clear NVS and reboot (wipes provisioning)

Scan node / Relay:
- `1` — print status (scanner ID, mesh address, app_idx, phase)
- `r` — reset mesh provisioning and reboot

---

## Common issues

- **`MQTT disconnected`** — Docker not running, wrong PC IP in `BMT_TB_HOST`, or Windows Firewall blocking port 1883.
- **Only one scan node visible** — all three flashed with the same Scanner ID. Flash each from its own folder.
- **Far scanner doesn't reach Gateway** — single-hop BLE Mesh is ~10 m indoors. Move the Gateway central or add the Relay.
- **Tag bounces between zones** — raise `BMT_ZONE_HYSTERESIS_DBM` from 5 to 8–10.
- **Build error: `esp_read_mac` undeclared** — add `#include "esp_mac.h"` (ESP-IDF v6.0 dropped the implicit include).

---

## Tech stack

ESP-IDF v6.0 · BLE Mesh (Vendor Model) · MQTT 3.1.1 · ThingsBoard CE 3.7.0 · Kalman filter · Path-loss distance model (n=2.5)

---

## Roadmap

- [x] BLE Mesh provisioning + Vendor Model
- [x] 3 scan nodes with time-division radio
- [x] Nearest-scanner zone detection
- [x] ThingsBoard integration with auto-provisioned sub-devices
- [ ] ThingsBoard dashboard (floor plan + timeline)
- [ ] Trilateration for sub-room positioning

## Contact & Support

<p style="font-size: 20px;"><strong>Cao Trong Phuoc</strong> - Software Engineer - Embedded Systems</p>

``` Note
Thank you for visiting this repository.
If you have any questions, suggestions, or feedback about this project or firmware development, feel free to contact me directly.
```

<a href="https://github.com/caotrongphuoc">
  <img src="https://img.shields.io/badge/GitHub-caotrongphuoc-181717?style=for-the-badge&logo=github&logoColor=white"/>
</a>

<a href="https://www.linkedin.com/in/cao-trong-phuoc/">
  <img src="https://img.shields.io/badge/LinkedIn-Cao%20Trong%20Phuoc-0A66C2?style=for-the-badge&logo=linkedin&logoColor=white"/>
</a>
