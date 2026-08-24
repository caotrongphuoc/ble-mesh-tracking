# Quickstart

Common flow first, then OS-specific notes. Assumes you have a workstation on the same LAN as the boards you flash.

## What you need

- ESP-IDF v6.0.1.
- Docker (Desktop on Windows, native on Linux).
- Python 3.
- Git.
- **3x ESP32-S3** (gateway + relay + tag) and **3x ESP32** (3 scanners). Six boards total. See the hardware table in the [README](../README.md#hardware) for the reasoning behind the split (S3 for BLE + WiFi coexistence on gateway/relay/tag; plain ESP32 is enough for the scanner role).
- One USB-serial cable per board.
- Optional: 1x nRF52840 board (nice!nano v2, XIAO BLE Sense) if you want to run the coin-cell battery Beacon variant of the tag - see [apps/Beacon_ProMicroNrf52840/README.md](../apps/Beacon_ProMicroNrf52840/README.md) and [apps/Beacon_XiaoNrf52840/README.md](../apps/Beacon_XiaoNrf52840/README.md).

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

## 3. Start ThingsBoard

Install Docker first. Linux: `sudo apt install docker.io docker-compose-plugin`. Windows: Docker Desktop (needs WSL2 on first run).

The repo ships no key material. Generate the dev CA + server cert once (`openssl` required - Git Bash on Windows already has it), then bring the stack up:

```
cd thingsboard
bash tls/gen_certs.sh          # first-run only; also copies ca.pem into both firmware EMBED_TXTFILES paths
docker compose up -d
```

Wait 1-2 minutes. Open `http://localhost:8080`, log in with `tenant@thingsboard.org` / `tenant`, change the password.

Device profiles, rule chain, dashboard, and gateway token: see [thingsboard.md](04-thingsboard.md). Do these before flashing.

## 4. Set firmware config

Edit `apps/*/components/bmt_config/bmt_config.h`:

| Define | Where | Value |
|---|---|---|
| `BMT_WIFI_SSID` / `BMT_WIFI_PASS` | gateway, scanner, relay | Your WiFi. |
| `BMT_TB_IP` | gateway | Docker host IP. |
| `BMT_TB_GATEWAY_TOKEN` | gateway | Token from step 3. |
| `BMT_OTA_*_URL` | gateway, scanner, relay | `https://<host-ip>:8443/<name>.bin`. |

Regenerating TLS certs with `tls/gen_certs.sh` refreshes both firmware `ca.pem` embeds (`apps/gateway/components/bmt_mqtt/ca.pem` and `components/bmt_ota/ota_ca.pem`) as part of the same run. Rebuild every affected firmware after a regen. See the *TLS trust chain* section of [thingsboard.md](04-thingsboard.md#tls-trust-chain).

## 5. Build and flash

Find the serial ports first: Linux `ls /dev/ttyUSB*`, Windows Device Manager under "Ports (COM & LPT)".

All four apps have Secure Boot V2 and Flash Encryption on. **Every board needs `erase-flash` on its first flash**, not just the gateway - that first boot is what burns the signing/encryption eFuses, and it is permanent per chip. Read [secure-boot.md](07-secure-boot.md) before you flash real hardware, and generate your own `keys/bmt_fleet_rsa3072.pem` first (not committed to the repo).

```
cd apps/gateway && idf.py -p <port> erase-flash flash
cd apps/scanner && idf.py -p <port> erase-flash flash
cd apps/relay   && idf.py -p <port> erase-flash flash
cd apps/tag     && idf.py -p <port> erase-flash flash
```

After that first flash, plain `idf.py -p <port> flash` is enough for the same board - it stays signed/encrypted with the same key.

Each build copies its `.bin` into `firmware/`. Override with `idf.py -DBMT_OTA_DIR=/some/dir build`.

Linux permission denied on `/dev/ttyUSB*`: `sudo usermod -aG dialout $USER`, then log out and back in.

## 6. Run

1. Power up the gateway first. Gateway is in AUTO mode and provisions each node on its own.
2. Power up the relay, wait for it to fully provision, **then** power up scanners **one at a time**, waiting for each to finish before powering the next. Provisioning is event-driven: the gateway handles one node's provision/config sequence (`APP_KEY_ADD` -> `MODEL_APP_BIND`) at a time, and powering multiple unprovisioned nodes together can cause some of them to miss their config step and never reach "fully configured".
3. Open the gateway serial monitor at 115200: `idf.py -p <port> monitor`. Press `1` to see the node table - confirm every node shows `Config done: YES` before moving to the next one.
4. Power up the tag(s) last. Tags do not provision (they only beacon), so order relative to them does not matter.
5. Open the Indoor Tracking dashboard in ThingsBoard.

If a node never reaches "fully configured", power-cycle just that node - it will send a fresh unprovisioned beacon and the gateway re-provisions it (see [operation.md](05-operation.md#self-healing)).

Full command list: [operation.md#uart-commands](05-operation.md#uart-commands). Test procedures: [testing.md](06-testing.md).

## 7. OTA

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

The nginx OTA fileserver comes up automatically with `docker compose up -d` in step 3 (it is one of the services in the stack). It serves `firmware/` over HTTPS on port `8443` using the same TLS cert as MQTTS. To restart just it:

```
cd thingsboard && docker compose up -d ota-fileserver
```

On gateway UART: `u` starts OTA for scanners and relays, `g` for gateway self-update. Full procedure: [testing.md#ota](06-testing.md#ota).

## Troubleshooting

- **Gateway stuck at "MQTT connecting..."** - wrong `BMT_TB_IP` or `BMT_TB_GATEWAY_TOKEN`, or firewall blocks port 8883.
- **Scanners never provision** - check gateway is in AUTO mode (press `a`). Move the relay closer if scanners are far.
- **`idf.py flash` "port not found"** on Windows - reboot to pick up new USB drivers, or check Device Manager for a yellow triangle.
- **`docker compose up -d` fails** - on Linux, `sudo systemctl start docker`.
