# `secure_boot_keys/`

Signing key for ESP-IDF Secure Boot V2. Referenced from every ESP-IDF
app's `sdkconfig`:

```
CONFIG_SECURE_BOOT_SIGNING_KEY="../../secure_boot_keys/bmt_fleet_rsa3072.pem"
```

The key is `.gitignore`d — every deployment must generate its own.
See [docs/13-secure-boot.md](../docs/13-secure-boot.md) for the
full Secure Boot flow.

## Generate

```
espsecure.py generate_signing_key --version 2 --scheme rsa3072 \
    secure_boot_keys/bmt_fleet_rsa3072.pem
```

Requires ESP-IDF to be sourced (`. $IDF_PATH/export.sh`) so
`espsecure.py` is on PATH.

## What the key does

- Bootloader and app image are signed with this key at build time.
- The first flash burns a hash of the corresponding public key into
  eFuse; from then on, only images signed by this same key can boot.
- Loss of the key means no more updates for boards already burned.
  Keep the key offline once a board is fielded.
