<div align="center">

[![License: Apache-2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg?style=flat-square)](LICENSE)
![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0.1-red?style=flat-square)
![Target](https://img.shields.io/badge/target-ESP32%20%7C%20ESP32--S3-informational?style=flat-square)

</div>

# BLE Mesh Tracking

<center><img width="1920" height="900" alt="BLE Mesh Tracking" src="resources/images/ble-mesh-tracking-banner.png" />
</center>

<hr>

Room-level indoor tracking on ESP32 / ESP32-S3 with BLE Mesh and a self-hosted ThingsBoard CE.

Tags send BLE beacons. Scanners read signal strength. A relay forwards mesh packets. The gateway pushes raw data to ThingsBoard over MQTTS. A ThingsBoard rule chain turns RSSI into a room name.

New to the project? Start with [docs/00-quickstart.md](docs/00-quickstart.md) (one-page). For the ThingsBoard side in depth — device profiles, rule chain, dashboard aliases, `ZONE_MAP`, verification, common problems — see [docs/04-thingsboard-setup.md](docs/04-thingsboard-setup.md).

## Documentation

| File | Description |
|---|---|
| [docs/00-quickstart.md](docs/00-quickstart.md) | Clone, install ESP-IDF, run Docker, build and flash, verify. Linux and Windows. |
| [docs/01-architecture.md](docs/01-architecture.md) | System layout and how data moves between nodes. |
| [docs/02-ble-mesh.md](docs/02-ble-mesh.md) | BLE Mesh parts used in this project. |
| [docs/03-algorithms.md](docs/03-algorithms.md) | Kalman filter, HMAC, hysteresis, OTA compare, watchdog. |
| [docs/04-thingsboard-setup.md](docs/04-thingsboard-setup.md) | ThingsBoard install, device profiles, rule chain, dashboard. |
| [docs/05-thingsboard-mqtt.md](docs/05-thingsboard-mqtt.md) | MQTT topics, payload format, and RPC. |
| [docs/06-http-tls.md](docs/06-http-tls.md) | HTTPS OTA server and TLS cert flow. |
| [docs/07-operation.md](docs/07-operation.md) | Runtime behavior and per-file source description. |
| [docs/08-uart-commands.md](docs/08-uart-commands.md) | UART commands for each node. |
| [docs/09-testing.md](docs/09-testing.md) | 9 manual tests and regression baseline. |
| [docs/10-testing-ota.md](docs/10-testing-ota.md) | End-to-end OTA testing and fault injection. |
| [docs/11-checklist.md](docs/11-checklist.md) | Pre-commit, pre-release, deployment checklists. |
| [docs/12-changelog.md](docs/12-changelog.md) | Change log. |
| [docs/13-secure-boot.md](docs/13-secure-boot.md) | Secure Boot V2 and Flash Encryption: concept, fleet signing key, first-flash caveats. |
| [docs/14-nrf52840-beacon.md](docs/14-nrf52840-beacon.md) | Optional coin-cell tag variant on nRF52840 (ProMicro / XIAO). Build via Zephyr / west. |

## Hardware

| Node | Board | What it does |
|---|---|---|
| Tag | ESP32-S3 | Sends a BLE beacon every 500 ms. |
| Scanner x3 | ESP32 | Reads tag RSSI and sends `TAG_STATUS` over mesh. |
| Relay | ESP32-S3 | Forwards mesh packets between far scanners and the gateway. |
| Gateway | ESP32-S3 | Provisions mesh, forwards data to ThingsBoard, runs OTA. |

## Firmware layout

Four apps under `apps/`. Each is a standard ESP-IDF project with modules under `components/bmt_*/`.

| App | Modules | What it does |
|---|---|---|
| **gateway** | 14 (config, types, mesh, node_table, mac_cache, scan_list, mqtt, thingsboard, ota, wifi, watchdog, uart, zone, factory_reset) | Provisions the mesh, bridges data to ThingsBoard over MQTTS, runs OTA, resets the mesh if it goes silent. |
| **scanner** | 10 (config, types, auth, scan_core, tag_table, mesh, scan, ota, uart, factory_reset) | Reads tag BLE beacons, verifies HMAC, sends `TAG_STATUS` over mesh. |
| **relay** | 6 (config, types, mesh, ota, uart, factory_reset) | Forwards mesh packets at the Network Layer between far scanners and the gateway. Handles `RESET_CMD` and `OTA_TRIGGER`. |
| **tag** | 4 (config, auth, beacon, uart) | Sends a 24-byte BLE beacon every 500 ms with a sequence number and HMAC-16. |

Shared code (relay and scanner OTA) lives at repo root under `components/bmt_ota/`.

## Data flow

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

## License

Apache License 2.0. See [LICENSE](LICENSE) for the full text and [NOTICE](NOTICE) for third-party attributions.

You are free to use, modify, and redistribute this project for any purpose, including commercial, as long as you keep the copyright and license notices. The Apache-2.0 patent grant applies.

## Contact

<p><strong>Cao Trong Phuoc</strong> - Software Engineer - Embedded Systems</p>

<a href="https://github.com/caotrongphuoc">
  <img src="https://img.shields.io/badge/GitHub-caotrongphuoc-181717?style=for-the-badge&logo=github&logoColor=white"/>
</a>

<a href="https://www.linkedin.com/in/cao-trong-phuoc/">
  <img src="https://img.shields.io/badge/LinkedIn-Cao%20Trong%20Phuoc-0A66C2?style=for-the-badge&logo=linkedin&logoColor=white"/>
</a>
