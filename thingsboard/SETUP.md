# ThingsBoard CE setup for BMT (self-hosted, MQTTS)

Directory-local README for `thingsboard/`. Condensed English setup below — for the full step-by-step with troubleshooting see [../docs/04-thingsboard-setup.md](../docs/04-thingsboard-setup.md); for the full Vietnamese deployment walkthrough see [import.md](import.md); for daily operations (start/stop) see [Thingsboard.md](Thingsboard.md).

## Folder contents

```
docker-compose.yml  — ThingsBoard CE 3.7 + PostgreSQL + nginx OTA (port 8443)
dashboard/          — dashboard export, ready to import (6 widgets, includes OTA panel)
rulechain/          — rule chain exports (zone hysteresis + OTA attribute persist)
tls/                — CA + server cert (dev bundle, CN + SAN both = bmt-tb.local; firmware verifies CN)
```

## Manual UI steps

### 1. Install Docker

Docker Desktop on Windows / macOS. Native `docker` + `docker-compose-plugin` on Linux.

### 2. Start ThingsBoard

```
cd thingsboard
docker compose up -d
```

Wait 1-2 minutes on the first run (database init). Check with `docker compose ps`.

### 3. Log in

Open `http://localhost:8080`, log in as `tenant@thingsboard.org` / `tenant`, change the password.

### 4. Create two device profiles

Menu Device profiles > `+`. Names must match exactly:

- `ble_tag`
- `ble_mesh_node`

### 5. Import rule chains

Menu Rule chains > `+ Import`:

- `rulechain/ble_tag_zone_detection.json` — set as default rule chain for the `ble_tag` profile.
- `rulechain/ble_mesh_node.json` — set as default rule chain for the `ble_mesh_node` profile (persists `ota_last_result` and `ota_last_time` on each node).

### 6. Create the gateway device and copy the token

Menu Devices > `+ Add device`. Name it `bmt_gateway`, enable `Is gateway`. In the Credentials tab, copy the Access Token.

Paste it into `apps/gateway/components/bmt_config/bmt_config.h`:

```c
#define BMT_TB_GATEWAY_TOKEN "<paste token here>"
```

Also update `BMT_TB_IP` in the same file to the IP of the machine running Docker.

### 7. Import the dashboard

Menu Dashboards > `+ Import dashboard` > pick `dashboard/indoor_tracking.json`.

Map the entity aliases:

- `Selected Tag` — `Entity from dashboard state`; populated when a row in
  the tracked-tags table is clicked.
- `All Tags` — filter by `Device profile = ble_tag` and resolve multiple
  entities.
- `All Mesh Devices` — include the `default`, `ble_tag`, and
  `ble_mesh_node` profiles (used by the all-devices table).
- `Mesh Nodes` — include the `ble_mesh_node` and `default` profiles so the
  node-status table also includes the gateway.

In dashboard view mode, click a row in **Tracked Tags** to load that tag into
all current-zone, floor-plan, RSSI, and diagnostic widgets.

### 8. Rebuild and flash the gateway

After step 6, rebuild `apps/gateway` and flash. The gateway connects over MQTTS on port 8883 and auto-registers sub-devices under the right profile.

## Regenerate certs

```
cd tls
bash gen_certs.sh
cp ca.pem ../../apps/gateway/components/bmt_mqtt/ca.pem
```

Rebuild the gateway. `EMBED_TXTFILES` bundles the new `ca.pem` into the firmware.

## Quick check after everything is up

- Gateway serial log prints `MQTTS -> mqtts://<ip>:8883 (verify CN=bmt-tb.local)` then `MQTT connected to ThingsBoard`. TLS handshake errors show up here as mbedTLS messages.
- TB UI Devices tab shows `bmt_gateway` online. Sub-devices
  (`bmt_node_<12-hex-MAC>`, `bmt_tag_0x<4-hex-ID>`) appear as scanners,
  relays, and tags come online.
- Indoor Tracking dashboard updates in real time. The OTA Status table lists each mesh node with online state and last OTA result.
