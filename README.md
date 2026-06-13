# BMT — BLE Mesh Tracking

Indoor positioning system using **BLE Mesh** on ESP32. Tags (ESP32 or iPhone iBeacon) are detected by scan nodes via RSSI. Data is forwarded through BLE Mesh to a Gateway, then bridged to **ThingsBoard** over MQTT. The Gateway determines which zone a tag is in by picking the scanner with the strongest signal (nearest-scanner zone detection with hysteresis).

---

## Architecture

```
                                BLE Advertising
       ┌──────────────┐  ───────────────────────►  ┌──────────────┐
       │   iPhone     │       iBeacon              │ Scan Node 1  │
       │  (iBeacon)   │  ───────────────────────►  │  (Bedroom 1) │
       │              │       iBeacon              │              │
       │      or      │  ───────────────────────►  ├──────────────┤
       │              │                            │ Scan Node 2  │
       │  ESP32 Tag   │  ───────────────────────►  │  (Bedroom 2) │
       │   (custom)   │                            ├──────────────┤
       └──────────────┘  ───────────────────────►  │ Scan Node 3  │
                                                   │   (Toilet)   │
                                                   └──────┬───────┘
                                                          │ BLE Mesh
                                                          │ (Vendor Model)
                                                          ▼
                                                   ┌──────────────┐
                                                   │  (optional)  │
                                                   │  Relay Node  │
                                                   └──────┬───────┘
                                                          │ BLE Mesh
                                                          ▼
                                                   ┌──────────────┐
                                                   │   Gateway    │
                                                   │   (ESP32 +   │
                                                   │   WiFi STA)  │
                                                   └──────┬───────┘
                                                          │ MQTT
                                                          │ (Gateway API)
                                                          ▼
                                                   ┌──────────────┐
                                                   │ ThingsBoard  │
                                                   │   (Docker)   │
                                                   │   Dashboard  │
                                                   └──────────────┘
```

**Data flow:**
1. **Tag** (iPhone iBeacon or custom ESP32) broadcasts BLE advertisements containing UUID + Major + Minor (tag ID).
2. **Scan nodes** continuously scan for advertisements, filter packets belonging to the system, then apply Kalman filtering to RSSI and estimate distance via path-loss model.
3. Each scan node publishes `{scanner_id, tag_id, rssi, distance, loss_pct}` to the Gateway over BLE Mesh Vendor Model (8-byte unsegmented payload).
4. **Gateway** receives reports from all scanners, runs zone detection logic (nearest scanner with 5 dBm hysteresis).
5. Gateway forwards telemetry plus the `zone` field to **ThingsBoard** via MQTT Gateway API.
6. ThingsBoard renders the real-time dashboard with floor plan and zone indicator.

---

## Hardware

| Role | Model | Quantity | Notes |
|---|---|---|---|
| Gateway | ESP32 NodeMCU-32S | 1 | Needs WiFi access to the ThingsBoard server |
| Scan Node | ESP32 DevKitC WROOM-32 | 3 | Placed in 3 rooms |
| Relay (optional) | ESP32 DevKitC WROOM-32 | 0–1 | Extends mesh range across walls / long distances |
| Tag | iPhone (nRF Connect app) | — | Or an ESP32 running the `bmt_tag` firmware |

**Server requirements:**
- Host machine running Docker (Windows / Linux / macOS)
- Docker Desktop or native Docker Engine
- ~2 GB free RAM for the ThingsBoard container

---

## Repo structure

```
ble-mesh-tracking/
├── bmt_gateway/         # Gateway firmware (Provisioner + MQTT bridge + Zone detection)
├── bmt_relay/           # Relay firmware (passive forwarder, optional)
├── bmt_scan_node_1/     # Scan node, ID=0x01 → zone "bedroom_1"
├── bmt_scan_node_2/     # Scan node, ID=0x02 → zone "bedroom_2"
├── bmt_scan_node_3/     # Scan node, ID=0x03 → zone "toilet"
├── bmt_tag/             # ESP32 tag firmware (optional — can use iPhone iBeacon instead)
├── docker-compose.yml   # ThingsBoard self-host stack (PostgreSQL backend)
└── README.md
```

Each firmware folder is a standalone ESP-IDF project. Reason for splitting `scan_node_1/2/3` into separate folders (instead of one project + build configs): each node has a different UUID and Scanner ID hardcoded in source, making `idf.py flash` straightforward without `menuconfig`.

---

## Naming convention

The codebase uses the `bmt_` prefix (**B**LE **M**esh **T**racking):

| Item | Pattern | Example |
|---|---|---|
| Macro | `BMT_<CATEGORY>_<NAME>` | `BMT_WIFI_SSID`, `BMT_TB_HOST` |
| Function | `bmt_<module>_<action>()` | `bmt_mqtt_init()`, `bmt_zone_evaluate()` |
| Struct | `bmt_<scope>_<name>_t` | `bmt_gateway_node_t`, `bmt_tag_report_t` |
| Global | `g_bmt_<name>` | `g_bmt_tag_track`, `g_bmt_mqtt_client` |

**ThingsBoard device naming:**
- `bmt_gateway` (role=`gateway`)
- `bmt_node_0xXXXX` (role=`relay` or `scan` — single naming scheme for both, differentiated by the `role` attribute)
- `bmt_tag_0xYYYY` (role=`tag`)

---

## Quick start

### 1. Start ThingsBoard

```bash
docker compose up -d
# First run takes ~2 minutes to initialize
docker compose logs -f thingsboard
# Wait until you see "Started ThingsboardServerApplication"
```

Open **http://localhost:8080**, login with `tenant@thingsboard.org` / `tenant`.

### 2. Create the Gateway device on ThingsBoard

1. Sidebar → **Entities** → **Devices** → **+** → **Add new device**
2. Name: `bmt_gateway`
3. **Enable the "Is gateway" toggle** ✅
4. Save → click the device → **Details** tab → **Copy access token**

### 3. Configure the Gateway firmware

Edit `bmt_gateway/main/main.c`, set the USER CONFIG macros:

```c
#define BMT_WIFI_SSID            "YourWiFiSSID"
#define BMT_WIFI_PASS            "YourWiFiPassword"
#define BMT_TB_HOST              "mqtt://<PC_IP>:1883"
#define BMT_TB_GATEWAY_TOKEN     "<paste_token_here>"
```

`<PC_IP>` is the LAN IP of the machine running Docker. The ESP32 and PC must be on the same subnet (same WiFi router, or PC on Ethernet + ESP32 on WiFi if the router covers both).

### 4. Build & flash the Gateway

```bash
cd bmt_gateway
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

Expected log:
```
[BMT_GW] WiFi connected! IP: ...
[BMT_GW] MQTT connected to ThingsBoard
[BMT_GW] TB Gateway ONLINE (role=gateway)
[BMT_GW] === BMT Gateway v2 READY ===
```

### 5. Flash the 3 scan nodes

Each scan node lives in its own folder with `BMT_SCANNER_ID` and UUID already set:

```bash
cd bmt_scan_node_1   # Scanner ID=0x01 → "bedroom_1"
idf.py -p COMx flash monitor

cd bmt_scan_node_2   # Scanner ID=0x02 → "bedroom_2"
idf.py -p COMy flash monitor

cd bmt_scan_node_3   # Scanner ID=0x03 → "toilet"
idf.py -p COMz flash monitor
```

**Important — provision sequentially:** flash and power on one scan node at a time, wait for the Gateway log `=== Scan node 0x000X READY ===` before moving to the next. The Gateway provisioner allocates unicast addresses in order, so flashing in parallel can race and produce duplicate addresses.

### 6. (Optional) Flash the Relay if mesh range is insufficient

For long or wall-divided layouts where single-hop BLE Mesh can't reach all scanners, place one Relay between Gateway and far scan nodes:

```bash
cd bmt_relay
idf.py -p COMx flash monitor
```

The Relay is auto-provisioned by the Gateway the same way scan nodes are.

### 7. Test with iPhone iBeacon

1. Install **nRF Connect** on iPhone
2. **Advertiser** tab → create a new iBeacon:
   - UUID: any value (UUID prefix check is disabled for iBeacons from iPhone)
   - **Major: `0x0001`** (PERSON) or **`0x0002`** (ASSET) — required, the scan node filters on this field
   - Minor: any value (becomes `tag_id | 0x8000` in the system)
3. Start advertising (▶ icon)
4. Walk through the rooms with the iPhone. On the ThingsBoard UI → device `bmt_tag_0xXXXX` → **Latest telemetry** tab:
   ```json
   {
     "scanner": "0x02",
     "type": "ASSET",
     "rssi": -65,
     "distance": 2.5,
     "loss": 0,
     "zone": "bedroom_2",
     "zone_id": "0x02"
   }
   ```

---

## Configuration details

### Vendor Model (BLE Mesh protocol)

| Field | Value |
|---|---|
| Company ID (CID) | `0x02E5` (Espressif) |
| Server Model ID | `0x0000` (Scan node — publisher) |
| Client Model ID | `0x0001` (Gateway — subscriber) |
| Opcode | `BMT_OP_VND_TAG_STATUS` = `ESP_BLE_MESH_MODEL_OP_3(0x00, 0x02E5)` |
| Payload size | 8 bytes (unsegmented, no ACK required) |

### Tag report payload (8 bytes)

```c
typedef struct {
    uint8_t  scanner_id;    /* 1B : Scanner ID that produced this report  */
    uint8_t  tag_type;      /* 1B : 0x01=PERSON, 0x02=ASSET               */
    uint16_t tag_id;        /* 2B : Tag ID (iPhone: minor | 0x8000)       */
    int8_t   rssi;          /* 1B : Filtered RSSI (dBm)                   */
    int16_t  distance_dm;   /* 2B : Distance × 10 (decimeters, 0.1m resolution) */
    uint8_t  loss_pct;      /* 1B : Packet loss rate (0-100%)             */
} bmt_tag_report_t;
```

### Zone detection params (Gateway)

| Param | Value | Meaning |
|---|---|---|
| `BMT_ZONE_HYSTERESIS_DBM` | 5 | Only change zone if the new RSSI exceeds the current one by ≥ 5 dBm |
| `BMT_SCANNER_VALID_MS` | 3500 | Scanner data older than 3.5 s is ignored when voting |
| `BMT_TAG_OUT_OF_RANGE_MS` | 10000 | If no scanner reports the tag for 10 s, zone becomes `out_of_range` |

### Zone mapping

Hardcoded in `bmt_gateway/main/main.c`:

```c
static const char *bmt_zone_name(uint8_t scanner_id)
{
    switch (scanner_id) {
    case 0x01: return "bedroom_1";
    case 0x02: return "bedroom_2";
    case 0x03: return "toilet";
    case 0xFF: return "out_of_range";
    default:   return "unknown";
    }
}
```

To change the mapping, edit this function and reflash the Gateway.

### Time-division radio (scan node)

An ESP32 has a single BLE radio, so mesh transmission and GAP scanning can't run simultaneously. The scan node uses a 1.5 s cycle:

```
|<-- GAP scan 1000ms -->|<-- Mesh publish 500ms -->|
   Sniff iBeacons           Publish to Gateway
   Kalman filtering         via BLE Mesh
```

Result: each tag is published once per 1.5 s per scanner. With 3 scanners, the Gateway receives ~6 telemetry events per second per tag (when all 3 can see it).

---

## UART commands

On the serial monitor (`idf.py monitor`):

### Gateway

| Key | Action |
|---|---|
| `1` | List provisioned nodes (UUID, MAC, addr, online status) |
| `2` | List tracked tags + current zone + RSSI from each scanner |
| `s` | Manual scan for 10 s, looking for unprovisioned beacons |
| `p` | Provision manually from the scan list |
| `a` | Enable auto-provision mode |
| `4` | Print menu help |
| `0` | **Clear NVS** + reboot (wipes all provisioning) |

### Scan node / Relay

| Key | Action |
|---|---|
| `1` | Print status (Scanner ID, node addr, app_idx, phase) |
| `r` | Reset mesh provisioning + reboot (back to unprovisioned) |

---

## Troubleshooting

### Gateway can't reach ThingsBoard (`MQTT disconnected`)
- Container running? → `docker ps`
- `BMT_TB_HOST` IP matches the Docker host LAN IP (not `localhost`)?
- Windows Firewall blocking port 1883? → add an inbound rule
- Token correct (copied from the `bmt_gateway` device on TB UI)?

### Three scan nodes flashed but only one shows on the Gateway
- Did all three use the same `BMT_SCANNER_ID`? → each must be flashed from a different folder (`bmt_scan_node_1/2/3`)
- Is the last byte of the UUID different per node? → in the source, byte 15 must match `BMT_SCANNER_ID`
- Provisioning race during parallel flash? → clear Gateway NVS (`0` key), then power on one scan node at a time

### Scan node far from Gateway → zone never changes
- This is the **physical limit of single-hop BLE Mesh** (~10 m indoors, ~50 % reduction per wall)
- Workarounds: move Gateway to a central location, or add a Relay (`bmt_relay`) between Gateway and the distant scan node
- Sanity check: place the distant scan node next to the Gateway temporarily — if it works, the issue is range

### Tag bounces between zones constantly
- RSSI noise exceeds the hysteresis threshold → raise `BMT_ZONE_HYSTERESIS_DBM` from 5 to 7–10
- Or slow down the update rate by raising `BMT_GAP_SCAN_DURATION_MS`

### Build error: `esp_read_mac` undeclared
- ESP-IDF v6.0 no longer auto-includes `esp_mac.h` via `esp_system.h`
- Fix: add `#include "esp_mac.h"` at the top of any file using `esp_read_mac()` / `ESP_MAC_BT`

### Build error: `esp-mqtt` component not found
- In ESP-IDF v6.0, `esp-mqtt` is a managed component, not a core component
- Already declared in `idf_component.yml`:
  ```yaml
  dependencies:
    espressif/mqtt: "*"
  ```

---

## Tech stack

- **Firmware:** ESP-IDF v6.0, C, FreeRTOS
- **Protocol:** BLE Mesh (Bluetooth SIG), Vendor Model, MQTT v3.1.1
- **Server:** ThingsBoard CE 3.7.0 (Docker, PostgreSQL backend)
- **DSP:** Kalman filter for RSSI smoothing, path-loss model (n=2.5) for distance estimation
- **Build env:** ESP-IDF VSCode extension (Espressif IDF v2.1.0)

---

## Roadmap

- [x] WiFi + MQTT bridge on Gateway
- [x] BLE Mesh provisioning + Vendor Model
- [x] 3 scan nodes with time-division radio
- [x] Zone detection (nearest-scanner with hysteresis)
- [x] ThingsBoard integration (Gateway API + auto-provision sub-devices)
- [ ] ThingsBoard dashboard (floor plan + timeline)
- [ ] Multi-tag tracking + alarm rules
- [ ] Triangulation for sub-room positioning (RSSI from 3 scanners → coordinates within a room)
