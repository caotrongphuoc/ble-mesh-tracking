# Secure Boot and Flash Encryption

Applies to `apps/gateway`, `apps/relay`, `apps/scanner`, `apps/tag` (all ESP32 / ESP32-S3). Not `apps/beacon` - separate board, still experimental, out of scope.

## Why

Before this, anyone with physical UART/USB access to a board could dump the flash and read everything in plaintext: WiFi password, ThingsBoard gateway token, the mesh NetKey/AppKey, the tag HMAC key. They could also flash a modified, unsigned firmware image. Secure Boot stops the second problem, Flash Encryption stops the first.

## What is enabled

- Secure Boot V2, RSA-3072 signature (`CONFIG_SECURE_BOOT_V2_RSA_ENABLED=y`). App and bootloader images are signed at build time (`CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y`).
- Flash Encryption, AES-128, key generated into eFuse on first boot (`CONFIG_SECURE_FLASH_ENC_ENABLED=y`, `CONFIG_SECURE_FLASH_ENCRYPTION_KEY_SOURCE_EFUSES=y`).
- Flash Encryption **Development mode** (`CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT=y`), not Release. See below.
- A new `nvs_key` partition in every `partitions.csv`:

  ```
  nvs_key,   data,  nvs_keys, ,        0x1000, encrypted
  ```

  **Why it has to exist.** Flash Encryption transparently encrypts the app and bootloader partitions, but it does *not* transparently cover NVS - NVS has its own wear-levelled read/write layout, so ESP-IDF encrypts it with a separate AES-XTS scheme instead. That scheme needs its own key set (subtype `nvs_keys`), and that key set has to be stored somewhere on flash: this partition. The `encrypted` flag means the key set itself is protected by Flash Encryption, so the NVS keys never sit on flash in the clear.

  **What happens if it is missing.** Three cases, all bad:
  - NVS encryption is on (as here) but there is no `nvs_keys` partition to hold the keys - `nvs_flash_init()` has nowhere to read them from and fails at boot. The node cannot bring up NVS at all.
  - You drop the partition and also turn NVS encryption off to make it build - then the `nvs` partition is written in plaintext. WiFi password, ThingsBoard token, NetKey/AppKey and the HMAC key are all readable again by anyone who dumps flash, which is exactly the hole Flash Encryption was added to close.
  - You remove the partition from a board that was already provisioned with encrypted NVS - it can no longer decrypt its own stored data, so it loses the node table and mesh keys and comes up as if factory-reset.

- NVS encryption itself (`CONFIG_NVS_ENCRYPTION=y`, `CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC=y`). The `nvs` partition is encrypted using the key held in the `nvs_key` partition above. This is what actually protects the WiFi password, ThingsBoard token, and mesh keys at rest.

## Other sdkconfig changes this pulls in

Two more `sdkconfig` values changed on all four apps alongside the security options - one is a hard requirement, one is a workaround. Both are easy to miss in a diff but matter if you compare partition layouts or debug a boot loop:

- **Partition table offset moved `0x8000` -> `0x10000`** (`CONFIG_PARTITION_TABLE_OFFSET`). The Secure Boot V2 bootloader carries a signature block, which makes it larger than the default gap before `0x8000`. ESP-IDF requires the partition table to start at `0x10000` to leave room. If you hand-edit a `partitions.csv` offset, keep this in mind - the first partition still starts after `0x10000`, not `0x8000`. And whatever else you change, do not drop the `nvs_key` line described above - a build without it either fails to init NVS or silently falls back to plaintext NVS (see the consequences under "What is enabled").
- **SPI flash frequency lowered `80m` -> `40m`** (`CONFIG_ESPTOOLPY_FLASHFREQ`). Our boards did not boot reliably at 80 MHz once Secure Boot and Flash Encryption were on - intermittent boot failures. Dropping to 40 MHz fixed it. This is a stability workaround for the specific dev boards used here, not a hard requirement of Secure Boot; on boards with better flash/wiring you may be able to keep 80 MHz. If you change board hardware, re-test at 80 MHz before assuming you need 40.

## One signing key for the whole fleet

All four apps point to the same key file:

```
CONFIG_SECURE_BOOT_SIGNING_KEY="../../keys/bmt_fleet_rsa3072.pem"
```

One RSA-3072 keypair signs gateway, relay, scanner, and tag firmware. A board only boots firmware signed by this key. Losing the key does not brick already-flashed boards - they keep running - but you lose the ability to sign and OTA any future firmware to them.

The key is gitignored and never committed. Generate your own before your first flash:

```
mkdir -p keys
espsecure.py generate_signing_key --version 2 --scheme rsa3072 keys/bmt_fleet_rsa3072.pem
```

Back it up outside the repo (password manager, offline drive). There is no recovery if you lose it after boards are in the field.

## Development vs Release mode

This repo ships with `CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT=y` and `CONFIG_SECURE_BOOT_ALLOW_JTAG=y`. That is intentional for bring-up: encryption is on, but JTAG and the UART download-mode re-flash path stay open so a misbehaving board can still be recovered on the bench. `sdkconfig` itself lists these under "Potentially insecure options" - that label is correct. Do not ship this configuration to a real deployment. For production, switch to `CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE` and close JTAG/download-mode, understanding that the eFuse burn at that point is permanent for that physical chip.

## First flash: what actually happens

1. `idf.py -p <port> erase-flash flash` builds a bootloader signed with your key and flashes it.
2. On first boot, the bootloader burns the `SECURE_BOOT_EN` eFuse. This is permanent for that chip - it survives `erase-flash`.
3. The same boot generates the flash encryption key and encrypts flash content in place.
4. From then on, that board only accepts app images signed by the same RSA key. Treat a board that has been through steps 2-3 as provisioned: a plain unsigned build will not boot on it again.

By design, once the secure-boot eFuse is burned the bootloader rejects an unsigned or wrongly-signed image at boot rather than running it - that is the whole point of Secure Boot. If you are not sure whether a board has already gone through first boot, watch its serial log while re-flashing: the exact rejection message is printed by the ROM bootloader. (Capture that message from your own board and paste it here - it is the most useful thing to have when debugging a failed flash, and it is board/ROM specific enough that it is worth recording the real string rather than a paraphrase.)

## Signing keys

Both signing keys live at repo root under `keys/`. **Neither is committed** (`.gitignore`d), and the folder itself does not exist in a fresh clone - create it before the first build:

```
mkdir -p keys
```

| File | Used by | Referenced in |
|---|---|---|
| `bmt_fleet_rsa3072.pem` | ESP-IDF Secure Boot V2 (all four ESP-IDF apps) | `apps/{gateway,scanner,relay,tag}/sdkconfig` -> `CONFIG_SECURE_BOOT_SIGNING_KEY` |
| `bmt_tag_ecdsa_p256.pem` | MCUboot (nRF52840 Beacon variants) | `apps/Beacon_*/sysbuild.conf` -> `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` |

The MCUboot key is independent of Secure Boot V2 - the two live side by side because both bootloaders read them from the same `keys/` folder.

### Generate `bmt_fleet_rsa3072.pem` (Secure Boot V2)

```
espsecure.py generate_signing_key --version 2 --scheme rsa3072 \
    keys/bmt_fleet_rsa3072.pem
```

Requires ESP-IDF to be sourced (`. $IDF_PATH/export.sh`) so `espsecure.py` is on PATH.

### Generate `bmt_tag_ecdsa_p256.pem` (MCUboot)

```
imgtool keygen -t ecdsa-p256 -k keys/bmt_tag_ecdsa_p256.pem
```

`imgtool` ships with Zephyr / MCUboot; it is on PATH after `west init` + `west update` on a Zephyr workspace. Full Beacon build flow in [../apps/Beacon_ProMicroNrf52840/README.md](../apps/Beacon_ProMicroNrf52840/README.md) and [../apps/Beacon_XiaoNrf52840/README.md](../apps/Beacon_XiaoNrf52840/README.md).

### Consequences of losing a key

- **Secure Boot V2**: the first flash burns a hash of the corresponding public key into eFuse; from then on, only images signed by this same key can boot on that board. Losing the key means no more updates for boards already burned. Keep the key offline once a board is fielded.
- **MCUboot**: the corresponding public key is baked into the MCUboot bootloader build. Losing the key means no more OTA updates for boards running that bootloader - regenerate before shipping.

Related: [quickstart.md](00-quickstart.md), [thingsboard.md#tls-trust-chain](04-thingsboard.md#tls-trust-chain), [operation.md#checklists](05-operation.md#checklists).
