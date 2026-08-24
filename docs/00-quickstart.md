# Quickstart

Common flow first, then OS-specific notes. Assumes you have a workstation on the same LAN as the boards you flash.

## What you need

**Toolchains:**

- **ESP-IDF v6.0.1** - gateway, relay, scanner, and the optional ESP32-S3 bench tag.
- **Zephyr + west** (recent LTS) - the primary Tag on nRF52840 XIAO. Install via the [Zephyr getting-started guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html).
- **Docker** (Desktop on Windows, native on Linux) - ThingsBoard CE + nginx OTA server.
- **Python 3** - `espsecure.py` and `imgtool` (both come with their respective toolchains once installed).
- **Git**.

**Boards** (5 ESP32 + 1 nRF52840, see the Hardware section of the [README](../README.md#i-hardware)):

- **2x ESP32-S3-DevKitC-1** - one flashed as gateway, one as relay.
- **3x ESP32-DevKitC-V4** - the three scanners.
- **1x Seeed XIAO nRF52840** - the coin-cell Tag (primary). ProMicro nRF52840 works too; see [apps/Beacon_ProMicroNrf52840/README.md](../apps/Beacon_ProMicroNrf52840/README.md).
- *Optional bench tag:* one extra **ESP32-S3-DevKitC-1** if you want to run [apps/tag/](../apps/tag/) side by side with the XIAO to compare beacon output.

**Cables and power:**

- One USB(-serial) cable per ESP32 board.
- One USB-C cable for the XIAO tag (also acts as the flashing interface).
- A LiPo or LIR coin cell for the tag if you want to run it off-battery after flashing.

## 1. Clone

```
git clone https://github.com/caotrongphuoc/ble-mesh-tracking.git
cd ble-mesh-tracking
```

## 2. Install ESP-IDF

### Linux

```
mkdir -p ~/esp && cd ~/esp
git clone -b v6.0.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32,esp32s3
. ./export.sh
```

Add `. ~/esp/esp-idf/export.sh` to your shell rc to get `idf.py` in every session.

### Windows

Download the ESP-IDF Installer v6.0.1 from `https://dl.espressif.com/dl/esp-idf/` and run it. Pick a short path like `C:\esp`. The installer creates a Start Menu shortcut "ESP-IDF v6.0.1 CMD" that opens a shell with `idf.py` on PATH.

## 3. Generate signing keys

Two signing keys are required before the first firmware build. **Neither is committed** - `keys/*.pem` is `.gitignore`d, and the `keys/` folder does not exist in a fresh clone. Create it, then run both commands from the repo root:

```
mkdir -p keys
```

**Secure Boot V2** (`gateway`, `relay`, `scanner`, and the bench `apps/tag`). Requires ESP-IDF to be sourced first so `espsecure.py` is on PATH (`. ~/esp/esp-idf/export.sh`):

```
espsecure.py generate_signing_key --version 2 --scheme rsa3072 keys/bmt_fleet_rsa3072.pem
```

**MCUboot** (nRF52840 Tag). Requires Zephyr / west installed (`imgtool` ships with the Zephyr Python venv):

```
imgtool keygen -t ecdsa-p256 -k keys/bmt_tag_ecdsa_p256.pem
```

Back both keys up outside the repo (password manager, offline drive). If you lose the Secure Boot key after a board has been flashed with Secure Boot on, that board can no longer receive a new signed image; see [secure-boot.md](07-secure-boot.md#one-signing-key-for-the-whole-fleet) and [keys/README.md](../keys/README.md).

## 4. Start ThingsBoard

Install Docker first. Linux: `sudo apt install docker.io docker-compose-plugin`. Windows: Docker Desktop (needs WSL2 on first run).

The repo ships no key material. Generate the dev CA + server cert once (`openssl` required - Git Bash on Windows already has it), then bring the stack up:

```
cd thingsboard
bash tls/gen_certs.sh          # first-run only; also copies ca.pem into both firmware EMBED_TXTFILES paths
docker compose up -d
```

Wait 1-2 minutes. Open `http://localhost:8080`, log in with `tenant@thingsboard.org` / `tenant`, change the password.

> **Do this before step 6 (flashing).** In [thingsboard.md](04-thingsboard.md), create the two device profiles, import the `ble_tag_zone_detection` rule chain, load the Indoor Tracking dashboard, and copy the gateway access token. If you skip these, the gateway will still connect via MQTTS but the dashboard stays empty (no rule chain to map RSSI to a room), which looks exactly like a broken deployment.

## 5. Set firmware config

Edit `apps/*/components/bmt_config/bmt_config.h`:

| Define | Where | Value |
|---|---|---|
| `BMT_WIFI_SSID` / `BMT_WIFI_PASS` | gateway, scanner, relay | Your WiFi. |
| `BMT_TB_IP` | gateway | Docker host IP. |
| `BMT_TB_GATEWAY_TOKEN` | gateway | Token from step 4. |
| `BMT_OTA_*_URL` | gateway, scanner, relay | `https://<host-ip>:8443/<name>.bin`. |

Regenerating TLS certs with `tls/gen_certs.sh` refreshes both firmware `ca.pem` embeds (`apps/gateway/components/bmt_mqtt/ca.pem` and `components/bmt_ota/ota_ca.pem`) as part of the same run. Rebuild every affected firmware after a regen. See the *TLS trust chain* section of [thingsboard.md](04-thingsboard.md#tls-trust-chain).

## 6. Build and flash

Two flash flows: ESP-IDF for the gateway/relay/scanner (and optional bench tag), then Zephyr + UF2 for the nRF52840 XIAO tag.

Find serial ports first: Linux `ls /dev/ttyUSB*`, Windows Device Manager under "Ports (COM & LPT)".

### 6a. Gateway, Relay, Scanner (ESP-IDF)

The three ESP-IDF apps have Secure Boot V2 and Flash Encryption on. **Every board needs `erase-flash` on its first flash**, not just the gateway - that first boot burns the signing / encryption eFuses, and it is permanent per chip. Read [secure-boot.md](07-secure-boot.md) before flashing real hardware; the RSA-3072 key you generated in [step 3](#3-generate-signing-keys) is what signs these images.

```
cd apps/gateway && idf.py -p <port> erase-flash flash
cd apps/scanner && idf.py -p <port> erase-flash flash
cd apps/relay   && idf.py -p <port> erase-flash flash
```

After that first flash, plain `idf.py -p <port> flash` is enough for the same board - it stays signed / encrypted with the same key.

Each build copies its `.bin` into `firmware/`. Override with `idf.py -DBMT_OTA_DIR=/some/dir build`.

Linux permission denied on `/dev/ttyUSB*`: `sudo usermod -aG dialout $USER`, then log out and back in.

### 6b. Tag - nRF52840 XIAO (Zephyr + MCUboot)

The primary Tag runs on the coin-cell **Seeed XIAO nRF52840** with a signed MCUboot image. Build produces a `.hex` and a merged UF2:

```
cd apps/Beacon_XiaoNrf52840
west build -b xiao_ble/nrf52840 -d build . --pristine
```

Sign and flash via UF2 - **do NOT flash the raw `zephyr.uf2`**, MCUboot rejects it silently and the app never boots. Full flow (including the sanity-check table for signed-vs-unsigned file size, entering the bootloader by double-tapping RESET, and the "board looks dead" recovery) is in [apps/Beacon_XiaoNrf52840/README.md](../apps/Beacon_XiaoNrf52840/README.md).

The ECDSA-P256 key you generated in [step 3](#3-generate-signing-keys) is what signs the MCUboot image.

### 6c. Optional: bench tag on ESP32-S3 (ESP-IDF)

Only needed if you want the ESP32-S3 reference build for A/B testing beacon output. Same flash flow as the other ESP-IDF apps:

```
cd apps/tag && idf.py -p <port> erase-flash flash
```

Skip this if you are only deploying the XIAO tag.

## 7. Run

1. Power up the gateway first. Gateway is in AUTO mode and provisions each node on its own.
2. Power up the relay, wait for it to fully provision, **then** power up scanners **one at a time**, waiting for each to finish before powering the next. Provisioning is event-driven: the gateway handles one node's provision/config sequence (`APP_KEY_ADD` -> `MODEL_APP_BIND`) at a time, and powering multiple unprovisioned nodes together can cause some of them to miss their config step and never reach "fully configured".
3. Open the gateway serial monitor at 115200: `idf.py -p <port> monitor`. Press `1` to see the node table - confirm every node shows `Config done: YES` before moving to the next one.
4. Power up the tag last. Plug the XIAO tag into USB (or a charged LiPo / LIR coin cell on the B+ pin - see [apps/Beacon_XiaoNrf52840/README.md](../apps/Beacon_XiaoNrf52840/README.md) for battery notes). Tags only beacon; they do not provision, so order relative to them does not matter.
5. Open the Indoor Tracking dashboard in ThingsBoard.

If a node never reaches "fully configured", power-cycle just that node - it will send a fresh unprovisioned beacon and the gateway re-provisions it (see [operation.md](05-operation.md#self-healing)).

Full command list: [operation.md#uart-commands](05-operation.md#uart-commands). Test procedures: [testing.md](06-testing.md).

## 8. OTA

### Where OTA `.bin` files come from

The repo does **not** ship pre-built firmware images. `firmware/*.bin` is gitignored on purpose - pre-built binaries drift out of sync with source (private WiFi/IP baked in, wrong TLS keys, etc.).

`.bin` files appear in `firmware/` automatically as a side-effect of building each app:

```
cd apps/gateway && idf.py build     # -> firmware/Gateway.bin
cd apps/scanner && idf.py build     # -> firmware/Scanner.bin
cd apps/relay   && idf.py build     # -> firmware/Relay.bin
```

The copy step is a CMake `POST_BUILD` command in each app (see `apps/*/CMakeLists.txt`). To send the `.bin` elsewhere, override:

```
idf.py -DBMT_OTA_DIR=/some/dir build
```

Rebuild any time you change config (`bmt_config.h`) or source - otherwise the node that boots after OTA still runs the old code.

### Serving OTA over HTTPS

The nginx OTA fileserver comes up automatically with `docker compose up -d` in step 4 (it is one of the services in the stack). It serves `firmware/` over HTTPS on port `8443` using the same TLS cert as MQTTS. To restart just it:

```
cd thingsboard && docker compose up -d ota-fileserver
```

On gateway UART: `u` starts OTA for scanners and relays, `g` for gateway self-update. Full procedure: [testing.md#ota](06-testing.md#ota).

## Troubleshooting

- **Gateway stuck at "MQTT connecting..."** - wrong `BMT_TB_IP` or `BMT_TB_GATEWAY_TOKEN`, or firewall blocks port 8883.
- **Scanners never provision** - check gateway is in AUTO mode (press `a`). Move the relay closer if scanners are far.
- **`idf.py flash` "port not found"** on Windows - reboot to pick up new USB drivers, or check Device Manager for a yellow triangle.
- **`docker compose up -d` fails** - on Linux, `sudo systemctl start docker`.
