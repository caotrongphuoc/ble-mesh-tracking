# Checklists

Short bullet lists to catch mistakes before they cost you a re-flash trip.

## Pre-commit code

Before `git commit`:

- [ ] `clang-format --dry-run --Werror apps/*/components/*/*.c apps/*/main/*.c` returns clean.
- [ ] All 4 apps build: `tools/build-all.sh`.
- [ ] `grep -R "TODO\|FIXME\|XXX" apps/` -- either finish them or file an issue.
- [ ] No stray `printf` debug lines left in hot paths (mesh callback, radio manager, MQTT worker).
- [ ] Any new `esp_err_t`-returning call is checked, or the return is deliberately ignored with `(void)`.
- [ ] New heap allocations (`malloc`, `calloc`, `xTaskCreate`) have a matching free path or documented lifetime.
- [ ] New static shared state has either a mutex, an atomic, or a comment explaining why it does not need one.
- [ ] Any change to `bmt_types.h` was mirrored across gateway + scanner + relay (same struct layout + opcode number).

## Pre-commit docs

- [ ] No emoji or icon characters. Sanity check: `grep -Pn "[\x{1F300}-\x{1FAFF}]" docs/ README.md` returns nothing.
- [ ] No filler or marketing phrasing (`In this section we will explore how...`, `robust and elegant solution...`). Direct sentences only.
- [ ] Links to other docs use the numbered filenames (`01-architecture.md`, not `architecture.md`).
- [ ] Any new algorithm has a code pointer (file + function).
- [ ] Any UART command mentioned exists in [08-uart-commands.md](08-uart-commands.md).
- [ ] Any config define mentioned exists in `bmt_config.h` of the app it belongs to.

## Pre-flash config (before shipping to a customer)

- [ ] `BMT_WIFI_SSID` and `BMT_WIFI_PASS` set to real credentials, not placeholders.
- [ ] `BMT_TB_IP` matches the customer's ThingsBoard host IP.
- [ ] `BMT_TB_GATEWAY_TOKEN` matches the token from ThingsBoard UI. Not the demo token.
- [ ] `BMT_OTA_GATEWAY_URL`, `BMT_OTA_SCANNER_URL`, `BMT_OTA_RELAY_URL` all start with `https://` and are reachable on port 8443 from the deployed LAN.
- [ ] `apps/gateway/components/bmt_mqtt/ca.pem` matches `thingsboard/tls/ca.pem` on the server (`diff` returns nothing).
- [ ] `components/bmt_ota/ota_ca.pem` matches `thingsboard/tls/ca.pem` on the server too — same CA, different embed location.
- [ ] `BMT_TB_CN` and `BMT_OTA_SERVER_CN` both match the CN inside the server cert (`openssl x509 -in server.pem -text | grep CN`).
- [ ] `BMT_TAG_MASTER_KEY` regenerated (not the dev default). Same value in tag AND scanner (both derive the epoch key from it — see [03-algorithms.md](03-algorithms.md#4a-tag-key-totp-style-epoch-derivation)).
- [ ] `BMT_MESH_STATIC_OOB_VAL` regenerated (not the dev default). Same value in gateway AND scanner AND relay.
- [ ] `CONFIG_BLE_MESH_SETTINGS=y` in gateway `sdkconfig`.
- [ ] Firmware `PROJECT_VER` shows the deployment build time. Sanity: it must be later than every previously flashed version.
- [ ] `secure_boot_keys/bmt_fleet_rsa3072.pem` generated yourself and backed up outside the repo before the first `erase-flash`. See [13-secure-boot.md](13-secure-boot.md) — the eFuse burn on first boot is permanent per board.

## Pre-release

Before tagging a release:

- [ ] All items in Pre-commit code and Pre-commit docs pass.
- [ ] Test 1 (bring-up) from [09-testing.md](09-testing.md) passes.
- [ ] Test 3 (walking) passes.
- [ ] Test 6 (gateway persistence) passes.
- [ ] Test 9 smoke from [09-testing.md](09-testing.md) or full flow in [10-testing-ota.md](10-testing-ota.md) passes.
- [ ] Changelog entry in [12-changelog.md](12-changelog.md) describes what changed and why.
- [ ] Tag the commit with the same date the build produces (`YYYYMMDD`).

## Deployment (on-site)

At the customer site, in order:

- [ ] ThingsBoard container running: `docker compose ps` shows `Up`.
- [ ] Certs regenerated for this deployment (see [04-thingsboard.md#regenerate-certs](04-thingsboard.md#regenerate-certs)).
- [ ] Firmware flashed with real credentials (see Pre-flash above).
- [ ] Gateway UART shows `MQTT connected to ThingsBoard` within 30 seconds of boot.
- [ ] Relay powered on and provisioned (`[RLY_CFG] Relay 0x00xx fully configured`).
- [ ] All scanners provisioned (`[SCN_CFG] Scan node 0x00xx fully configured`).
- [ ] Tag(s) powered on and visible on the dashboard.
- [ ] `ZONE_MAP` in the ThingsBoard rule chain updated with the actual scanner MACs (read them off UART with command `1`).
- [ ] A full walking test in every room shows the correct zone assignment.
- [ ] HTTPS OTA server running for future updates: `cd thingsboard && docker compose up -d ota-fileserver`.

## After a bug is found in production

- [ ] Reproduce locally with the failing tag/scanner/relay layout.
- [ ] Add a note in [12-changelog.md](12-changelog.md) describing the symptom.
- [ ] Fix the root cause, not the symptom. Adding another `if` to paper over a race is a symptom fix.
- [ ] Add or update a test in [09-testing.md](09-testing.md) so the same regression is caught next time.
- [ ] OTA the fix to every node in the field. Verify via ThingsBoard `ota_result` attribute.
