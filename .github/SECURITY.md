# Security

## Reporting a vulnerability

Please **do not** open a public GitHub issue for security problems.

Report privately through GitHub Security Advisories on this
repository:

- Go to the **Security** tab → **Advisories** → **Report a vulnerability**.
- Or open the URL directly:
  `https://github.com/caotrongphuoc/ble-mesh-tracking/security/advisories/new`.

Public disclosure will be coordinated once a fix is available.

## Supported versions

Only the latest release on `main` receives security fixes. This is a
personal / research-scale project, not a long-term-supported product.

| Version | Supported |
|---|---|
| `main` (current) | Yes |
| `1.0.x` | Yes |
| `< 1.0` | No |

## Key material in this repository — important caveat

Cryptographic material was committed to this repository during
pre-1.0 development and was **not** rewritten out of the git history
during the open-source cleanup. Anyone who can run
`git clone --mirror` on this repo and read `git log -p` can recover
every historical value:

- `keys/bmt_fleet_rsa3072.pem` — Secure Boot V2 signing
  key (all four ESP-IDF apps).
- `keys/bmt_tag_ecdsa_p256.pem` — MCUboot signing key for the
  nRF52840 Beacon variants.
- `thingsboard/tls/ca.key` and `thingsboard/tls/server.key` —
  self-signed dev CA and the ThingsBoard / OTA server key.
- Two 16-byte HMAC keys defined in
  `apps/*/components/bmt_auth/bmt_auth.c` — `BMT_TAG_MASTER_KEY` and
  `BMT_OTA_BEACON_HMAC_KEY`.

Current `main` uses fresh key material generated after the cleanup,
so the running code on your working tree is not the leaked one.
Treat every historical value as public regardless.

**If you plan to run this on real hardware, regenerate every one of
those before flashing.** Skipping regeneration means:

- Anyone on the LAN can forge a TLS server the firmware would trust.
- Anyone can sign a firmware image the bootloader would accept
  (Secure Boot / MCUboot).
- Anyone can forge tag beacons or OTA-trigger beacons that pass HMAC
  verification.

### Regenerate

| Key | Command |
|---|---|
| Secure Boot V2 (ESP-IDF, all four apps) | `espsecure.py generate_signing_key --version 2 --scheme rsa3072 keys/bmt_fleet_rsa3072.pem` |
| MCUboot (nRF52840 Beacon apps) | `imgtool keygen -t ecdsa-p256 -k keys/bmt_tag_ecdsa_p256.pem` |
| TLS CA + server cert (MQTTS + OTA nginx) | `cd thingsboard && bash tls/gen_certs.sh` |
| `BMT_TAG_MASTER_KEY` / `BMT_OTA_BEACON_HMAC_KEY` | Replace the 16 bytes in each `apps/*/components/bmt_auth/bmt_auth.c` (must be byte-for-byte identical across Tag, Scanner and Beacon apps for `BMT_TAG_MASTER_KEY`) |

More per-key detail lives in [`keys/README.md`](../keys/README.md),
[`keys/README.md`](../keys/README.md) and
[`thingsboard/tls/gen_certs.sh`](../thingsboard/tls/gen_certs.sh).

## Third-party dependencies

Security issues in ESP-IDF, Zephyr / MCUboot, Mbed TLS, NimBLE,
FreeRTOS, ThingsBoard CE, or nginx should be reported upstream. See
[`NOTICE`](../NOTICE) for links.
