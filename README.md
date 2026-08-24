<div align="center">

![Repo Traffic](https://komarev.com/ghpvc/?username=ble-mesh-tracking&label=Repo+Traffic&color=blue&style=flat-square)

</div>

<p align="center">
  <img src="https://img.shields.io/badge/language-C-red?style=flat-square&logo=c" alt="Language">
  <img src="https://img.shields.io/badge/mcu-ESP32%20%7C%20ESP32--S3-red?style=flat-square" alt="MCU">
  <img src="https://img.shields.io/badge/framework-ESP--IDF%20v6.0.1-red?style=flat-square" alt="Framework">
  <img src="https://img.shields.io/badge/backend-ThingsBoard%20CE-red?style=flat-square" alt="Backend">
</p>

# BLE Mesh Tracking - Room-level Indoor Tracking on ESP32

<center><img width="1280" height="640" alt="BLE Mesh Tracking" src="docs/images/banner_ble_mesh_tracking_1280x640.png" />
</center>

<hr>

## Demo

<div align="center">
  <em>Demo video coming soon. The banner above shows the deployed hardware setup.</em>
</div>

## Documentation

| File | Description |
| --- | --- |
| [README.md](README.md) | Project overview, hardware, firmware layout, data flow. |
| [docs/00-quickstart.md](docs/00-quickstart.md) | Clone, install ESP-IDF, run Docker, build and flash, verify. Linux and Windows. |
| [docs/01-architecture.md](docs/01-architecture.md) | System layout and how data moves between nodes. |
| [docs/02-ble-mesh.md](docs/02-ble-mesh.md) | BLE Mesh parts used in this project. |
| [docs/03-algorithms.md](docs/03-algorithms.md) | Kalman filter, HMAC, hysteresis, OTA compare, watchdog. |
| [docs/04-thingsboard.md](docs/04-thingsboard.md) | ThingsBoard stack: install, profiles, rule chain, dashboard, MQTT topics, TLS trust, HTTPS OTA server. |
| [docs/05-operation.md](docs/05-operation.md) | Runtime behavior, UART commands, factory reset, source layout, checklists. |
| [docs/06-testing.md](docs/06-testing.md) | 16 tests: bring-up, walking, self-heal, watchdog, OTA end-to-end, regression baseline. |
| [docs/07-secure-boot.md](docs/07-secure-boot.md) | Secure Boot V2 and Flash Encryption: concept, fleet signing key, first-flash caveats. |
| [apps/Beacon_ProMicroNrf52840/README.md](apps/Beacon_ProMicroNrf52840/README.md) | Optional coin-cell tag variant on nRF52840 ProMicro (Zephyr / west). |
| [apps/Beacon_XiaoNrf52840/README.md](apps/Beacon_XiaoNrf52840/README.md) | Optional coin-cell tag variant on nRF52840 XIAO (Zephyr / west). |

## Introduction

Room-level indoor tracking on ESP32 / ESP32-S3 with BLE Mesh and a self-hosted ThingsBoard CE. Tags send BLE beacons, scanners read signal strength, a relay forwards mesh packets, the gateway pushes raw data to ThingsBoard over MQTTS, and a ThingsBoard rule chain turns RSSI into a room name.

The project puts several embedded engineering ideas into practice:

- **BLE Mesh:** Provisioning, vendor opcodes, network-layer relay, key rotation.
- **Secure firmware:** Secure Boot V2 (RSA-3072), Flash Encryption (AES-128), HMAC-16 beacon authentication.
- **Signal processing:** Adaptive Kalman filter on RSSI, hysteresis + leaky-bucket debounce on zone switching.
- **Server-side rules:** ThingsBoard rule chain that maps `scanner_id` to a room and pushes updates over WebSocket.
- **OTA over mesh:** HTTPS fileserver on nginx, per-node result reporting, SHA256 self-check on the gateway.

### I. Hardware

<table align="center">
  <tr>
    <td align="center"><img src="docs/images/banner_ble_mesh_tracking_1280x640.png" alt="BLE Mesh Tracking hardware" width="900"/></td>
  </tr>
</table>
<p align="center"><strong><em>Figure 1:</em></strong> Node roles across the deployed mesh</p>

<div align="center">

| Node | Board | What it does |
|---|---|---|
| Tag | ESP32-S3 | Sends a BLE beacon every 500 ms. |
| Scanner x3 | ESP32 | Reads tag RSSI and sends `TAG_STATUS` over mesh. |
| Relay | ESP32-S3 | Forwards mesh packets between far scanners and the gateway. |
| Gateway | ESP32-S3 | Provisions mesh, forwards data to ThingsBoard, runs OTA. |

</div>

Coin-cell battery variants of the Tag on **nRF52840** are documented in the two Beacon READMEs linked in the Documentation table.

### II. Firmware Layout

Four ESP-IDF apps under `apps/`. Each is a standard ESP-IDF project with modules under `components/bmt_*/`.

<div align="center">

| App | Modules | What it does |
|---|---|---|
| **gateway** | 14 (config, types, mesh, node_table, mac_cache, scan_list, mqtt, thingsboard, ota, wifi, watchdog, uart, zone, factory_reset) | Provisions the mesh, bridges data to ThingsBoard over MQTTS, runs OTA, resets the mesh if it goes silent. |
| **scanner** | 10 (config, types, auth, scan_core, tag_table, mesh, scan, ota, uart, factory_reset) | Reads tag BLE beacons, verifies HMAC, sends `TAG_STATUS` over mesh. |
| **relay** | 6 (config, types, mesh, ota, uart, factory_reset) | Forwards mesh packets at the Network Layer between far scanners and the gateway. Handles `RESET_CMD` and `OTA_TRIGGER`. |
| **tag** | 4 (config, auth, beacon, uart) | Sends a 24-byte BLE beacon every 500 ms with a sequence number and HMAC-16. |

</div>

Shared code (relay and scanner OTA) lives at repo root under [`components/bmt_ota/`](components/bmt_ota/).

### III. Data Flow

End-to-end path from a tag beacon to a highlighted room on the dashboard:

```mermaid
%%{init: {'theme':'base','themeVariables':{'fontSize':'18px','primaryColor':'#1565c0','primaryTextColor':'#ffffff','primaryBorderColor':'#0d47a1','lineColor':'#90a4ae','signalColor':'#ffc107','signalTextColor':'#ffc107','actorBkg':'#1565c0','actorBorder':'#0d47a1','actorTextColor':'#ffffff','actorLineColor':'#90caf9','noteBkgColor':'#fff59d','noteTextColor':'#000000','noteBorderColor':'#f57f17','activationBkgColor':'#66bb6a','activationBorderColor':'#2e7d32','sequenceNumberColor':'#ffffff','loopTextColor':'#ffc107','labelBoxBkgColor':'#37474f','labelBoxBorderColor':'#90a4ae','labelTextColor':'#ffffff'},'sequence':{'actorMargin':90,'messageFontSize':16,'noteFontSize':14,'actorFontSize':16,'boxMargin':12,'boxTextMargin':6,'noteMargin':10,'useMaxWidth':true}}}%%
sequenceDiagram
    autonumber
    participant T as Tag<br/>(ESP32-S3)
    participant S as Scanner<br/>(ESP32)
    participant R as Relay<br/>(ESP32-S3, optional hop)
    participant G as Gateway<br/>(ESP32-S3, addr 0x0001)
    participant TB as ThingsBoard CE<br/>(Docker)
    participant D as Dashboard<br/>(browser)

    loop Every 500 ms
        T->>S: BLE ADV 24 B<br/>UUID + major + minor + tx_power + seq + HMAC-16
    end
    Note over T,S: HMAC key derived from<br/>master + epoch (1 h, TOTP-style)

    S->>S: Verify HMAC + anti-replay (seq)<br/>Kalman filter RSSI (adaptive R)

    alt Scanner within direct mesh range of Gateway
        S->>G: BLE Mesh unicast<br/>vendor opcode TAG_STATUS
    else Scanner out of direct range
        S->>R: BLE Mesh TAG_STATUS
        R->>G: Network Layer forward<br/>(TTL - 1, no app processing)
    end

    G->>G: Parse {tag_id, rssi, scanner_id}<br/>Enqueue to MQTT worker (64-slot queue)
    G->>TB: MQTTS port 8883<br/>publish v1/gateway/telemetry

    Note over TB: Rule chain ble_tag_zone_detection:<br/>1) parse + map scanner_id -> room<br/>2) fetch tag state (current_zone, candidate)<br/>3) hysteresis 5 dBm vs current room<br/>4) leaky-bucket debounce (2 confirms)

    par Save attribute (state)
        TB->>TB: current_zone = "room_X"<br/>current_scanner, candidate_count
    and Save timeseries (history)
        TB->>TB: rssi_scan_01..03 for charts
    end

    TB-->>D: WebSocket push<br/>(attribute + timeseries update)
    D->>D: Highlight room on floor plan<br/>update RSSI chart
```

<p align="center"><strong><em>Figure 2:</em></strong> End-to-end data flow (Tag -> Scanner -> Relay -> Gateway -> ThingsBoard -> Dashboard)</p>

### IV. Runtime and OTA

- **Provisioning:** Gateway runs in AUTO mode and provisions each unprovisioned node one at a time with Static OOB auth. Details in [docs/02-ble-mesh.md](docs/02-ble-mesh.md).
- **Self-healing:** A 30-second window watchdog broadcasts `RESET_CMD` if the mesh goes silent, and nodes rejoin on their own. Full flow in [docs/05-operation.md](docs/05-operation.md#self-healing).
- **OTA over mesh:** Gateway broadcasts an OTA-beacon, each node downloads its `.bin` from the LAN HTTPS server (nginx). Test procedure in [docs/06-testing.md](docs/06-testing.md#ota).
- **Security:** Secure Boot V2 + Flash Encryption on all four apps, TOTP-style HMAC-16 on tag beacons, TLS with CN pinning on MQTTS. See [docs/07-secure-boot.md](docs/07-secure-boot.md).

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
