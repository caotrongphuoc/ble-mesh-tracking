# import.md — Deploy the BMT system from scratch (code → ThingsBoard → running)

This guide covers **standing up the whole system on a new machine**
(or from zero on the same one): fetch code, bring up the server,
import config, flash firmware, verify end-to-end. Each step has a
**"Why"** paragraph so nothing is done blindly.

> For everyday operation (start / stop ThingsBoard) see
> `Thingsboard.md`. MQTT API detail: `../docs/05-thingsboard-mqtt.md`.
> One-page English quickstart: `../docs/00-quickstart.md`.

---

## Part 0 — Big picture

Data flow once everything is running:

```
Tag ──ADV──→ Scanner ──mesh──→ Relay ──mesh──→ Gateway ──MQTTS──→ ThingsBoard ──→ Dashboard
```

Deployment goes right-to-left: **bring up the server first** (so the
Gateway has a token to use), **flash the Gateway last of the nodes**.
Master checklist:

- [ ] Host: Docker Desktop + Git (+ ESP-IDF v6.0.1 if building firmware)
- [ ] Clone the repo
- [ ] ThingsBoard running (`docker compose up -d`)
- [ ] 2 Device Profiles + 1 gateway device + token
- [ ] Import rule chain + set as default + import dashboard
- [ ] Edit `bmt_config.h` (WiFi, IP, token) → build → flash 6 boards
- [ ] Update `ZONE_MAP` with real scanner MACs
- [ ] End-to-end verification

---

## Part 1 — Prepare the host

Install in this order:

1. **Docker Desktop** — [docker.com](https://www.docker.com/products/docker-desktop/).
   After installing, Settings → General → tick *"Start Docker Desktop
   when you sign in"* (so the server comes up on boot).
2. **Git** — [git-scm.com](https://git-scm.com/).
3. **ESP-IDF v6.0.1** — only needed if this machine will build / flash
   firmware. Install via the
   [ESP-IDF Installation Manager](https://dl.espressif.com/dl/esp-idf/)
   or the VS Code "ESP-IDF" extension. *(Skip if you only run the
   server.)*
4. USB-serial driver for the board (CH340 / CP210x) if Windows does
   not enumerate a COM port.

**Why Docker?** ThingsBoard CE is Java + PostgreSQL — installing by
hand is fiddly. Docker packages both into two containers; one command
to run, easy to remove.

---

## Part 2 — Get the code

```bash
git clone https://github.com/caotrongphuoc/ble-mesh-tracking.git
cd ble-mesh-tracking
```

The repo already contains **everything** the server side needs:
`thingsboard/docker-compose.yml`, the dev TLS bundle
(`thingsboard/tls/`), the JSON exports
(`thingsboard/rulechain/`, `thingsboard/dashboard/`), and the docs
under `docs/`. Firmware source lives at
`apps/{gateway, scanner, relay, tag}` — each module is an ESP-IDF
component under the app's `components/` directory. The scanner and
relay share their OTA implementation via `components/bmt_ota` at the
repo root.

---

## Part 3 — Bring up ThingsBoard

```bash
cd thingsboard
docker compose up -d
```

Wait ~2 minutes (longer the first time — image download + database
init). Check:

```bash
docker compose ps          # 3 containers "running": thingsboard, tb-postgres, bmt-ota-server
docker compose logs -f tb  # look for "Started ThingsboardServerApplication"
```

The third container `bmt-ota-server` (nginx on port **8443 HTTPS**)
serves firmware `.bin` files out of the repo's `firmware/`
directory — **no more** manual `python -m http.server` per OTA. Build
a new firmware, the `.bin` lands in `firmware/`, nodes can download
it.

Open **http://localhost:8080**, log in with the default tenant:
- User: `tenant@thingsboard.org`
- Pass: `tenant` → **change the password immediately** (top-right → Profile).

**Why the `tls/` directory?** Gateway connects over
**MQTT + TLS (port 8883)** to encrypt data and authenticate the
server. Docker compose mounts the certs from `tls/` into the
container. The bundled certs are dev-ready. Notable design point:
firmware verifies against **Common Name `bmt-tb.local`**, not the IP
— so **switching machines or IPs does NOT require re-issuing certs**.
For a private cert set: `bash tls/gen_certs.sh`, then copy the new
`ca.pem` over `apps/gateway/components/bmt_mqtt/ca.pem` (Gateway
embeds the CA to verify the server).

---

## Part 4 — Configure ThingsBoard (the "import" section)

Do steps in **this order** — later steps depend on earlier ones.

### 4.1. Create the two Device Profiles

**Profiles → Device profiles → +** create two profiles, names must
match **character-for-character**:

| Name | For |
|---|---|
| `ble_tag` | Tracked tags |
| `ble_mesh_node` | Scanner / Relay (online / offline state) |

**Why create them first, why do the names matter?** Gateway does not
need you to create a device per tag / node — it **self-declares**
them over MQTT Gateway API (`v1/gateway/connect`) with a
`"type": "ble_tag"` / `"ble_mesh_node"` field (defined in
`apps/gateway/components/bmt_config/bmt_config.h` →
`BMT_PROFILE_TAG / NODE`). ThingsBoard looks up the profile **by name**
— a one-character mismatch drops the device into the `default`
profile, and the zone rule chain never runs for it.

### 4.2. Create the Gateway device and copy its token

**Entities → Devices → + Add new device**:
- Name: `bmt_gateway`
- Tick **"Is gateway"**
- After creating, open the device → **Details** tab → **Copy access token**.

**Why?** This is the single "account" the firmware needs: the ESP32
Gateway authenticates with this token, and every tag / child node
flows through it (ThingsBoard gateway architecture). The token gets
pasted into the firmware in Part 5.

### 4.3. Import the zone rule chain

**Rule chains → ⬆ (Import rule chain)** → pick
`thingsboard/rulechain/ble_tag_zone_detection.json`.
(Optional: also import `rulechain/ble_mesh_node.json` — surfaces
Scanner / Relay OTA results on the dashboard; set it as the default
rule chain for the `ble_mesh_node` profile.)

Open the imported rule chain — 7 nodes, main flow:

```
[Fetch tag state]──→[Apply hysteresis]──→[Build attrs payload]──→[Save attributes]
 (loads existing     (ZONE algorithm)        └──→[Build TS payload]──→[Save timeseries]
  server attrs)
```

**Why a dedicated rule chain?** This is the "brain" that used to live
in firmware. The **"Apply hysteresis"** node holds the whole
algorithm (TBEL script): pick the scanner with the strongest RSSI
among samples < 10 s old, only switch rooms when the new one wins by
≥ 5 dBm (hysteresis — prevents flapping when two scanners are close)
AND holds for 2 consecutive samples leaky-bucket-style (debounce —
tolerates isolated noise).

### 4.4. Set the rule chain as default for profile `ble_tag`

**Profiles → Device profiles → `ble_tag` → ✏ → Rule chain** →
select `ble_tag_zone_detection` → Save.

**Why is this required?** Importing a rule chain just places it in
the library. Which chain a device's telemetry flows into is decided
by the **device's profile**. Skip this step and tag telemetry only
passes through the Root chain (raw storage) — zone is never
computed. This is the easiest step to forget.

### 4.5. Import the dashboard

**Dashboards → ⬆ Import dashboard** → pick
`thingsboard/dashboard/indoor_tracking.json`.

The dashboard supports multiple tags through two aliases: `All Tags` resolves
every device in the `ble_tag` profile for the summary table, while `Selected
Tag` resolves the entity passed through dashboard state. In view mode, click a
row in **Tracked Tags** to update the floor plan, current-zone card, RSSI
charts, and diagnostics for that tag.

### 4.6. Update ZONE_MAP with real scanner MACs

Open the rule chain → double-click **"Apply hysteresis"** → find the
top of the script:

```js
var ZONE_MAP = {
    "9a01c6842178": "room_1",
    "12a60986e694": "room_2",
    "86eab91ad6b8": "room_3"
};
```

Replace the keys with **your scanner MACs** (lowercase, no `:`) and
set the values to the room labels you want to display. To read a
MAC: power the scanner, open the serial port at 115200, press `1` —
the `MAC` line is right there. Save the script (✓) then **Save rule
chain** (the red ✓ in the top-right).

**Why map here instead of in firmware?** This is the main benefit of
the new architecture: moving a scanner to a different room is a
one-line edit in the browser — no reflash.

---

## Part 5 — Configure the firmware

Edit `components/bmt_config/bmt_config.h` in each app
(`apps/gateway`, `apps/scanner`, `apps/relay`, `apps/tag`):

| Define | Project | Value |
|---|---|---|
| `BMT_WIFI_SSID` / `BMT_WIFI_PASS` | Gateway, Scanner, Relay | WiFi 2.4 GHz (ESP32 cannot see 5 GHz) |
| `BMT_TB_IP` | Gateway | **LAN IP of the Docker host** (find via `ipconfig`, e.g. `192.168.1.50`) |
| `BMT_TB_GATEWAY_TOKEN` | Gateway | The token copied in step 4.2 |
| `BMT_OTA_SCANNER_URL` etc. | Gateway, Scanner, Relay | `https://<that IP>:8443/<Name>.bin` — port **8443** HTTPS = the auto-started OTA server (nginx, service `ota-fileserver`), shares the self-signed CA with MQTTS. Do NOT use 8080 — that is the ThingsBoard Web UI. |

**Why the IP has to be edited but the cert does not?** The IP is
baked into firmware (embedded devices have no reliable local DNS), so
switching the server host means a rebuild + flash of the Gateway.
The cert is verified by CN, so it can stay as-is. Tag has no WiFi —
nothing to change there.

---

## Part 6 — Build + flash

Open the ESP-IDF terminal (ESP-IDF PowerShell / `export.bat`) and
flash each project to its COM port:

```bash
cd apps/gateway && idf.py -p COM11 erase-flash flash   # FIRST time MUST erase-flash
cd ../scanner   && idf.py -p COM19 flash               # SAME firmware for all 3 scanners
cd ../relay     && idf.py -p COM10 flash
cd ../tag       && idf.py -p COM22 flash
```

**Why must the Gateway be `erase-flash` first?** Gateway uses a
custom partition table (NVS = 64 KB instead of the default 24 KB —
holds mesh keys + node table). Flashing over a board with a different
layout without erasing leaves the old NVS in the wrong place; you get
garbage data. Subsequent flashes (same layout) do not need it — and
**should not** erase, or the mesh keys are lost and every node has
to be re-provisioned.

**Why do the 3 scanners share one firmware?** Scanner uses the chip's
MAC as its identity (the mesh UUID embeds the MAC), nothing is
hardcoded per board — so one `.bin` flashes all three, and later OTA
broadcasts hit all three in one shot.

---

## Part 7 — Run + end-to-end verification

Power all boards (order does not matter), then check the chain — if
anything is off, stop and fix it there:

1. **Gateway serial** (115200): expect `WiFi connected` →
   `MQTT connected to ThingsBoard`.
   - If you see `esp-tls: select() timeout`: ThingsBoard is not
     running, or `BMT_TB_IP` is wrong.
2. Gateway auto-provisions: `Provision complete addr=0x000X` for
   each of the 4 nodes, press `1` → all 4 show `ACTIVE` / `ONLINE`,
   and there is a `Node table saved to NVS (4 nodes)` line.
3. Regular `[VND] src=... MAC=... tag=0x0001 rssi=...` lines flow →
   mesh + tag OK.
4. In ThingsBoard **Entities → Devices**: `bmt_tag_0x0001` and
   `bmt_node_<12-hex-MAC>` appear automatically (Gateway self-declares, as
   explained in 4.1).
5. Open the tag device → **Attributes** → `current_zone` changes as
   you carry the tag to another room.
6. Open the *Indoor Tracking* dashboard → position updates live.

**Self-healing test (worth running to trust the system):** unplug the
Gateway for 10 s → plug it back in → the log should print
`Node table loaded (4 nodes)` → `NVS nodes detected — watching 30s`
→ `Mesh OK`, and data resumes on its own with no board touched.

---

## Part 8 — Common problems on a new deployment

| Symptom | Likely cause | Fix |
|---|---|---|
| Gateway prints `MQTT disconnected` repeatedly | TB not running / wrong `BMT_TB_IP` / wrong token | Check `docker compose ps`, ping the IP, re-check the token |
| Tag device ends up in the `default` profile | Profile was created after the Gateway connected, or the profile name is wrong | Delete that device on TB → Gateway recreates it under the right profile |
| Tag has telemetry but no `current_zone` | Missed step 4.4 (set default rule chain) | Set it, then wait for the next telemetry packet |
| Zone stays on the same room forever | `ZONE_MAP` MAC is wrong (remember: lowercase, no `:`) | Compare the MAC with `1` on the scanner serial |
| Node provisions but says `Failed to find Dst` after reboot | Gateway was reflashed without erasing (NVS layout drift) | `idf.py erase-flash flash` + press `r` on each node |
| Web UI on 8080 does not come up | TB still booting / Docker not running | `docker compose logs -f tb` to see progress |

---

## Part 9 — (Optional) carry historical data over

The two JSON exports only carry **logical config**, not **data**
(telemetry, position history). Data lives in the Docker PostgreSQL
volume. To move data too:

```bash
# Old host — back up the volume to a tarball:
docker run --rm -v thingsboard_postgres-data:/data -v ${PWD}:/backup alpine tar czf /backup/tb-data.tar.gz -C /data .

# New host — after `docker compose up -d`, STOP the stack and restore:
docker compose down
docker run --rm -v thingsboard_postgres-data:/data -v ${PWD}:/backup alpine sh -c "rm -rf /data/* && tar xzf /backup/tb-data.tar.gz -C /data"
docker compose up -d
```

(Find the volume name with `docker volume ls`.) Rarely needed for a
demo — the new deployment just starts writing fresh data.
