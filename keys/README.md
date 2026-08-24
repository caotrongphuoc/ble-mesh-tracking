# `keys/`

Cryptographic signing keys. The repo ships none - every deployment
must generate its own. Both `.pem` files are `.gitignore`d.

| File | Used by | Referenced in |
|---|---|---|
| `bmt_fleet_rsa3072.pem` | ESP-IDF Secure Boot V2 (all four ESP-IDF apps) | `apps/{gateway,scanner,relay,tag}/sdkconfig` → `CONFIG_SECURE_BOOT_SIGNING_KEY` |
| `bmt_tag_ecdsa_p256.pem` | MCUboot (nRF52840 Beacon variants) | `apps/Beacon_*/sysbuild.conf` → `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` |

## Generate

### `bmt_fleet_rsa3072.pem` (Secure Boot V2)

```
espsecure.py generate_signing_key --version 2 --scheme rsa3072 \
    keys/bmt_fleet_rsa3072.pem
```

Requires ESP-IDF to be sourced (`. $IDF_PATH/export.sh`) so
`espsecure.py` is on PATH. See
[`docs/07-secure-boot.md`](../docs/07-secure-boot.md) for the full
Secure Boot V2 flow.

### `bmt_tag_ecdsa_p256.pem` (MCUboot)

```
imgtool keygen -t ecdsa-p256 -k keys/bmt_tag_ecdsa_p256.pem
```

`imgtool` ships with Zephyr / MCUboot; it is on PATH after
`west init` + `west update` on a Zephyr workspace. See
[`apps/Beacon_ProMicroNrf52840/README.md`](../apps/Beacon_ProMicroNrf52840/README.md)
and [`apps/Beacon_XiaoNrf52840/README.md`](../apps/Beacon_XiaoNrf52840/README.md)
for the Beacon build flow.

## Consequences of losing a key

- **Secure Boot V2**: the first flash burns a hash of the
  corresponding public key into eFuse; from then on, only images
  signed by this same key can boot on that board. Losing the key
  means no more updates for boards already burned. Keep the key
  offline once a board is fielded.
- **MCUboot**: the corresponding public key is baked into the
  MCUboot bootloader build. Losing the key means no more OTA updates
  for boards running that bootloader - regenerate before shipping.
