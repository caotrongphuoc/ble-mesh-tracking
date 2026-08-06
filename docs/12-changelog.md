# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] — 2026-07-31

First open-source release. Firmware behavior unchanged from `[0.2.0]`; the
delta is documentation, licensing and community-file work.

### Added

- Apache License 2.0 (`LICENSE`) with third-party attributions in `NOTICE`.
- `CONTRIBUTING.md` covering build setup, code style, `[TAG] subject` commit
  convention and PR flow.
- `.github/workflows/build.yml` — matrix CI that builds all four ESP-IDF
  apps (`gateway`, `scanner`, `relay`, `tag`) and both Zephyr Beacon
  variants on every push and PR.
- `.github/ISSUE_TEMPLATE/` and `.github/PULL_REQUEST_TEMPLATE.md`.
- `tools/README.md` documenting `format.sh` and `build-all.sh`.
- `docs/14-nrf52840-beacon.md` — the previously undocumented coin-cell
  tag variant on Zephyr / west.
- Root `README.md` gained a Mermaid `sequenceDiagram` of the end-to-end
  data flow (Tag → Scanner → optional Relay → Gateway → ThingsBoard).
- `docs/01-architecture.md` gained a **Physical layout** section
  (scanner placement rules, when to add a relay) and a table mapping
  each layer to its location in the repo.

### Changed

- `docs/00-quickstart.md` — corrected the hardware list (3× ESP32-S3 +
  3× ESP32, previously wrong) and added an explicit "Where OTA `.bin`
  files come from" section.
- `docs/03-algorithms.md` Kalman description now matches the code:
  `q` is fixed, `r` is adaptive (EMA α = 0.1, clamped to `[1.0, 20.0]`).
- `docs/04-thingsboard-setup.md` and `docs/05-thingsboard-mqtt.md`
  clarify that firmware verifies TLS **CN**, not SAN; the server cert
  has both set to `bmt-tb.local`.
- `thingsboard/import.md` — fixed a wrong OTA URL
  (`http://…:8081` → `https://…:8443`) to match `docker-compose.yml`.
- `components/bmt_ota/bmt_ota.c` — the 180-line `ota_wifi_task` split
  into `ota_wifi_bring_up` / `ota_check_and_flash` / `ota_wifi_tear_down`
  helpers. Behavior unchanged.
- Whole repo now passes `clang-format --dry-run --Werror` against the
  root `.clang-format` (Allman, tabs, `ColumnLimit: 0`), including the
  Zephyr Beacon apps.

### Removed

- Legacy pre-refactor monolithic `gateway_main.c` and `scanner_main.c`
  (~2700 lines under `docs/legacy/`) — available in git history.
- All in-code `[FIX]/[ADD]/[REVERTED]/[TEST]` historical comment tags
  (~75 lines). Rule now documented in `CONTRIBUTING.md`.
- Accidentally committed `ble-mesh-tracking.zip` (nested self-copy) and
  pre-built `firmware/*.bin` binaries baked with a local LAN IP. Both
  patterns are now in `.gitignore`; `.bin` regenerates on `idf.py build`.

## [0.2.0] — 2026-07-18

Security hardening on top of `[0.1.0]`. Covers `apps/gateway`,
`apps/relay`, `apps/scanner`, `apps/tag`, shared `components/bmt_ota`
and `thingsboard/`.

### Added

- Secure Boot V2 (RSA-3072) on all four apps — bootloader and app
  image are signed at build time; an unsigned or tampered image will
  not boot. See `docs/13-secure-boot.md`.
- Flash Encryption (AES-128, Development mode) plus an `nvs_key`
  partition (`encrypted` flag) in every `partitions.csv`, so NVS
  content — mesh keys, HMAC key, WiFi and ThingsBoard credentials —
  is encrypted at rest.
- `bmt_factory_reset` component on gateway, relay and scanner:
  holding the BOOT button (GPIO0) for 10 s erases NVS and reboots.
  Firmware is untouched.

### Changed

- OTA fileserver moved from plain HTTP
  (`python -m http.server 8080`) to HTTPS on port 8443. An `nginx`
  container (`ota-fileserver` in `thingsboard/docker-compose.yml`)
  serves `firmware/`, reusing the same TLS cert / key pair as MQTTS.
  Rationale: `docs/06-http-tls.md`.
- Gateway now publishes `ota_result` to ThingsBoard before rebooting
  on both self-update success and failure — the attribute reflects
  gateway self-update, not just node OTA.
- Mesh node device identifier changed from mesh address
  (`bmt_node_0x%04x`, which changes on every re-provision) to
  MAC-based (`bmt_node_%02x%02x%02x%02x%02x%02x`, fixed per physical
  device). Fixes ThingsBoard accumulating a new device entry on each
  reset or re-provision.

## [0.1.0] — 2026-07-09

Initial system architecture, forked from the pre-2026-06-28 monolithic
prototype. That prototype had one big `main.c`, computed the zone on
the gateway, and required hand-assigning scanner IDs.

### Added

- MQTTS from gateway to ThingsBoard with a self-signed CA, verified by
  CN `bmt-tb.local` so IP changes do not require new certs.
- Random per-network NetKey and AppKey.
- Static OOB authentication during provisioning — a rogue device
  without the OOB value cannot join even if it fakes the UUID.
- HMAC-16 authentication on tag beacons with a 24 h key rotation
  distributed over mesh. Scanners drop out-of-order sequence numbers
  to block replays.
- Gateway watchdog: waits 15 s after boot, does not wipe if all five
  `RESET_CMD` sends fail, counts relay ACKs as "mesh alive", only
  `error_code == 0` counts.
- Automatic node rejoin — if a provisioned UUID reappears as
  unprovisioned, the gateway drops the old entry and re-provisions.

### Changed

- Gateway is now a data-forwarder only; zone logic lives in the
  ThingsBoard rule chain, which does hysteresis and leaky-bucket
  debounce. Stale RSSI falls back to the last shown value instead of
  `-999`.
- Scanners use their MAC as ID — one firmware fits every scanner.
  UART `1` prints the MAC.
- Gateway code split from one `main.c` into `bmt_*` modules.
- Turned on `CONFIG_BLE_MESH_SETTINGS=y` on the gateway so keys, node
  list, devkey and sequence numbers persist across power loss.
- Grew NVS from 24 KB to 64 KB. Added error logs for node-table save
  and load; warns clearly if NVS has to be erased (`NO_FREE_PAGES`).
- Grew the mesh publish buffer from 12 to 20 bytes so pushing the
  16-byte HMAC key no longer fails silently with
  `Too small publication msg size`.
- Only the relay is pinged. Pinging every node with a shared config
  client made ONLINE / OFFLINE flap.

### Fixed

- Race in node reset. `esp_ble_mesh_node_local_reset()` is async; the
  old fixed-delay-then-reboot sometimes left NVS partially erased.
  Nodes now wait for `PROV_RESET_EVT` before rebooting, with a 5 s
  fallback.
- "Gateway power loss kills the whole mesh" — each boot used to
  generate a fresh random NetKey, so nodes with the old key could no
  longer reach the gateway (`Failed to find Dst`). Fixed by the
  `CONFIG_BLE_MESH_SETTINGS` change above.
