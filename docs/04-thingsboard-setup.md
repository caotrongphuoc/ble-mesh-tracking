# ThingsBoard setup

Long-form ThingsBoard CE setup for BMT: what the `thingsboard/` folder
holds, how to bring up the server, and how to import every piece the
firmware expects. This is the authoritative reference — the one-page
end-to-end [quickstart](00-quickstart.md) links here for the details.

## What is in `thingsboard/`

- `docker-compose.yml` — three containers: ThingsBoard CE 3.7,
  PostgreSQL, and an nginx OTA fileserver on port 8443.
- `rulechain/` — rule chain exports:
  - `ble_tag_zone_detection.json` — zone algorithm (hysteresis +
    leaky-bucket debounce). Default chain for the `ble_tag` profile.
  - `ble_mesh_node.json` — persists `ota_last_result` and
    `ota_last_time` attributes per node. Default chain for the
    `ble_mesh_node` profile.
- `dashboard/indoor_tracking.json` — dashboard, ready to import.
  6 widgets, includes an OTA status panel.
- `tls/` — CA and server certs (both CN and SAN = `bmt-tb.local`;
  firmware verifies CN, so switching machines or IPs does not require
  re-issuing certs). See [06-http-tls.md](06-http-tls.md).

## Steps

Do them in order — later steps depend on earlier ones.

### 1. Install Docker

Docker Desktop on Windows / macOS. Native `docker` +
`docker-compose-plugin` on Linux.

### 2. Generate the TLS bundle (once)

The repo ships no key material. On a fresh clone, generate a dev CA +
server cert before starting the stack. The script also copies the new
`ca.pem` into both firmware `EMBED_TXTFILES` paths so the next
firmware build trusts this server:

```
cd thingsboard
bash tls/gen_certs.sh
```

Requires `openssl` (Git Bash on Windows already has it). Re-run any
time certs expire or if you change the hostname inside the script.

### 3. Start ThingsBoard

```
docker compose up -d
```

First run takes 1–2 minutes for image download and database init.
Check status:

```
docker compose ps          # 3 running: thingsboard, tb-postgres, bmt-ota-server
docker compose logs -f tb  # look for "Started ThingsboardServerApplication"
```

The third container `bmt-ota-server` (nginx on **HTTPS port 8443**)
serves firmware `.bin` files out of the repo's `firmware/` directory.
Build a new firmware, the `.bin` lands there, mesh nodes can download
it. No more manual `python -m http.server` per OTA.

### 4. Log in

Open `http://localhost:8080`, log in with `tenant@thingsboard.org` /
`tenant`, then change the password (top-right → Profile).

### 5. Create two device profiles

Menu Device profiles → `+`. Names must match **character-for-character**:

| Name | For |
|---|---|
| `ble_tag` | Tracked tags |
| `ble_mesh_node` | Scanner and Relay (online / offline state) |

You do not create a device per tag or node — the gateway self-declares
them over MQTT Gateway API (`v1/gateway/connect`) with a
`"type": "ble_tag"` or `"ble_mesh_node"` field defined in
`apps/gateway/components/bmt_config/bmt_config.h`
(`BMT_PROFILE_TAG` / `BMT_PROFILE_NODE`). ThingsBoard looks up the
profile **by name** — a one-character mismatch drops the device into
the `default` profile and the zone rule chain never runs for it.

### 6. Create the gateway device and copy the token

Menu Devices → `+ Add device`. Name it `bmt_gateway`, tick
**Is gateway**. In the Credentials tab, copy the Access Token.

Paste it into `apps/gateway/components/bmt_config/bmt_config.h`:

```c
#define BMT_TB_GATEWAY_TOKEN "<paste token here>"
```

Also update `BMT_TB_IP` in the same file to the LAN IP of the machine
running Docker.

### 7. Import the rule chains

Menu Rule chains → `+ Import`:

- `thingsboard/rulechain/ble_tag_zone_detection.json` — **set as
  default** for the `ble_tag` profile (Profiles → Device profiles →
  `ble_tag` → ✏ → Rule chain → Save).
- `thingsboard/rulechain/ble_mesh_node.json` — set as default for the
  `ble_mesh_node` profile.

Setting the default is easy to forget. Importing a rule chain just
places it in the library; **which chain a device's telemetry flows
into is decided by the device's profile**. Skip this step and tag
telemetry only passes through the Root chain (raw storage) — the
zone attribute is never computed.

### 8. Update `ZONE_MAP` with real scanner MACs

Open `ble_tag_zone_detection` → double-click the **Apply hysteresis**
node → find the top of the script:

```js
var ZONE_MAP = {
    "9a01c6842178": "room_1",
    "12a60986e694": "room_2",
    "86eab91ad6b8": "room_3"
};
```

Replace the keys with your scanner MACs (lowercase, no `:`) and set
the values to room labels. To read a MAC, power the scanner, open the
serial port at 115200, press `1` — the `MAC` line is right there.
Save the script (✓), then **Save rule chain** (top-right).

Editing `ZONE_MAP` in the browser instead of firmware is the main
benefit of this architecture: moving a scanner to a different room
is a one-line edit, no reflash.

### 9. Import the dashboard

Menu Dashboards → `+ Import dashboard` → pick
`thingsboard/dashboard/indoor_tracking.json`.

Map the entity aliases:

- `Selected Tag` — `Entity from dashboard state`; populated when a
  row in the tracked-tags table is clicked.
- `All Tags` — filter by `Device profile = ble_tag`, resolving
  multiple entities for the summary table.
- `All Mesh Devices` — include the `default`, `ble_tag`, and
  `ble_mesh_node` profiles (used by the all-devices table).
- `Mesh Nodes` — include the `ble_mesh_node` and `default` profiles
  so the node-status table also includes the gateway.

In view mode, click a row in **Tracked Tags** to load that tag into
the floor plan, current-zone card, RSSI charts, and diagnostic
widgets.

### 10. Rebuild and flash the gateway

After step 6, rebuild `apps/gateway` and flash. The gateway connects
over MQTTS on port 8883 and auto-registers sub-devices under the
right profile.

## Verification

Power all boards (order does not matter), then check the chain — if
anything is off, stop and fix it there:

1. **Gateway serial** at 115200 prints `WiFi connected` →
   `MQTTS -> mqtts://<ip>:8883 (verify CN=bmt-tb.local)` →
   `MQTT connected to ThingsBoard`.
2. Gateway auto-provisions: `Provision complete addr=0x000X` for each
   node. Press `1` — every node shows `ACTIVE` / `ONLINE` and a
   `Node table saved to NVS (N nodes)` line follows.
3. Regular `[VND] src=... MAC=... tag=0x0001 rssi=...` lines flow →
   mesh + tag OK.
4. **ThingsBoard → Entities → Devices**: `bmt_tag_0x0001` and
   `bmt_node_<12-hex-MAC>` appear automatically as the gateway
   self-declares them.
5. Open the tag device → **Attributes** → `current_zone` updates as
   the tag moves between rooms.
6. Open the *Indoor Tracking* dashboard → the position updates live
   and the OTA Status table shows each mesh node's online state and
   last OTA result.

**Self-heal check.** Unplug the gateway for 10 s → plug it back in →
the log should print `Node table loaded (N nodes)` →
`NVS nodes detected — watching 30s` → `Mesh OK`, and data resumes on
its own with no board touched.

## Regenerate certs

Re-run `bash thingsboard/tls/gen_certs.sh` (same command as step 2).
It refreshes `ca.pem` / `server.pem` in `thingsboard/tls/` and copies
the new `ca.pem` into both firmware `EMBED_TXTFILES` paths. Then
rebuild the gateway (and any node whose OTA CA must trust the new
server) so `EMBED_TXTFILES` bundles the fresh CA into the image, and
restart the Docker stack so the containers reload the server cert.

## Common problems on a new deployment

| Symptom | Cause | Fix |
|---|---|---|
| Gateway prints `MQTT disconnected` repeatedly | TB not running, wrong `BMT_TB_IP`, or wrong token | `docker compose ps`, ping the IP, re-check the token |
| Tag ends up in the `default` profile | Profile created after the gateway first connected, or a name typo | Delete that device — the gateway recreates it under the right profile |
| Tag has telemetry but no `current_zone` | Missed the "set default rule chain" part of step 6 | Set it, then wait for the next telemetry packet |
| Zone stuck on one room | `ZONE_MAP` MAC is wrong (case, colons) | Compare with `1` on the scanner serial |
| Node provisions but says `Failed to find Dst` after reboot | Gateway was reflashed without erasing (NVS layout drift) | `idf.py erase-flash flash` on the gateway, then `r` on each node |
| Web UI on 8080 does not come up | TB still booting or Docker not running | `docker compose logs -f tb` to watch progress |

## Optional: carry historical data across hosts

The two JSON exports carry logical config only — not telemetry or
position history. Data lives in the Docker PostgreSQL volume. To
migrate it too:

```bash
# Old host — back up the volume to a tarball:
docker run --rm -v thingsboard_postgres-data:/data -v ${PWD}:/backup alpine tar czf /backup/tb-data.tar.gz -C /data .

# New host — after `docker compose up -d`, stop the stack and restore:
docker compose down
docker run --rm -v thingsboard_postgres-data:/data -v ${PWD}:/backup alpine sh -c "rm -rf /data/* && tar xzf /backup/tb-data.tar.gz -C /data"
docker compose up -d
```

Find the volume name with `docker volume ls`. Rarely needed for a
demo — a new deployment just starts writing fresh data.
