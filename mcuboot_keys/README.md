# `mcuboot_keys/`

Signing key for MCUboot on the nRF52840 Beacon variants. Referenced
from both Beacon apps' `sysbuild.conf`:

```
SB_CONFIG_BOOT_SIGNATURE_KEY_FILE="../../mcuboot_keys/bmt_tag_ecdsa_p256.pem"
```

The key is `.gitignore`d — every deployment must generate its own.
See [docs/14-nrf52840-beacon.md](../docs/14-nrf52840-beacon.md) for
the Beacon build flow.

## Generate

```
imgtool keygen -t ecdsa-p256 -k mcuboot_keys/bmt_tag_ecdsa_p256.pem
```

`imgtool` ships with Zephyr / MCUboot. It is on PATH after `west init`
+ `west update` on a Zephyr workspace.

## What the key does

- MCUboot verifies the signature on each image slot before booting it.
- The corresponding public key is baked into the MCUboot bootloader
  build.
- Losing the key means no more OTA updates for boards running that
  bootloader — regenerate before shipping a board.
