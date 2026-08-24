# Beacon - XIAO nRF52840 (Seeed Studio)

BLE beacon tag firmware for the **Seeed XIAO nRF52840** board.

Sister app: [`../Beacon_ProMicroNrf52840`](../Beacon_ProMicroNrf52840)
for the ProMicro / nice!nano v2 board. **The beacon and auth logic is
identical byte-for-byte**; only the hardware-dependent parts differ
(partition layout, battery-read path, power configuration).

---

## 0. Overview

An **optional coin-cell variant** of [`../tag`](../tag) (ESP-IDF).
Same wire protocol byte-for-byte (CID `0x02E5`, 24-byte payload,
HMAC-16, TOTP-style epoch key) so ESP32 scanners recognise it out of
the box - no scanner-side change needed. Pick this variant when a
wearable coin-cell tag matters more than reflash-over-USB convenience;
stick with `apps/tag` on ESP32-S3 otherwise.

Build system is **Zephyr / west** (not ESP-IDF); see the
[Zephyr getting-started guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html).
The bootloader is **MCUboot** with ECDSA-P256 signing - the signing
key lives in `../../keys/`; the repo ships none. See
[`../../docs/07-secure-boot.md#signing-keys`](../../docs/07-secure-boot.md#signing-keys)
for the regen command.

For interop the master HMAC key must match byte-for-byte across every
tag variant and the Scanner - the 16 bytes live in
`apps/*/components/bmt_auth/bmt_auth.c` (`BMT_TAG_MASTER_KEY`) and
must be identical in every app that uses it.

**Status: experimental.** No OTA-over-mesh support (the OTA path in
`apps/tag` is not shared with this variant); flashing is manual via
UF2 as described below.

---

## 1. Build

```powershell
west build -b xiao_ble/nrf52840 -d build . --pristine
```

---

## 2. Flash - READ THIS SECTION CAREFULLY

The bootloader is **MCUboot** with ECDSA-P256 signature verification.
**Do NOT flash the app's raw `zephyr.uf2`** - that file is **UNSIGNED**;
MCUboot silently rejects it and the board never boots the app.

Symptoms of flashing an unsigned file (easy to mistake for "board is
dead"):

- The `XIAO-SENSE` drive disappears normally after copy (looks like
  a successful flash).
- **But no app COM port appears**, no BLE, no log at all.
- Reason for the silence: `mcuboot.conf` disables the bootloader's
  console, so it rejects the app without printing anything.

### How to flash

```powershell
.\make_uf2.ps1
```

The script produces `tag_Xiao_SIGNED.uf2`. Then:

1. **Double-tap RESET** to enter the bootloader.
2. The `XIAO-SENSE` drive appears -> copy the `.uf2` onto it.
3. The drive disappears on its own = the board reset and is now
   running the firmware.
4. Verify: a new COM port shows up + `Test beacon` is visible in
   **RNF Digital Innovation Beacon Toolkit** on iOS.

### Sanity-check the file before flashing

| Check | Correct | Wrong |
|---|---|---|
| File size | ~329 KB (merged) | ~270 KB (app only, unsigned) |
| First data block address | `0x27000` (MCUboot) | `0x33000` (app) |

### If the board does not boot / the bootloader is stuck

If the drive does not disappear after copy, or double-tap does not
work: **cut power completely** (unplug USB **and** disconnect the
battery) for ~10 s, then reconnect. A soft reset (double-tap) does
not clear the bootloader's stuck state; only a cold boot does.

---

## 3. Hardware status (as of 2026-07-25)

> **The current XIAO board has a BLOWN CHARGER IC (BQ25101).** Symptoms:
> the charge LED is on constantly whether powered by battery only or by
> USB only, the board heats up on its own, and abnormally high current
> is measured (~88 mA vs the expected ~12-20 uA).
>
> Cause: current was measured on the BAT+ rail **while USB was still
> connected** - the charger IC was pushing charge current through the
> same wire being measured, overloading and destroying the die.
>
> **LESSON: always unplug USB before wiring a multimeter into the
> battery rail.**
>
> This board should not be powered (by battery or USB) until the
> charger IC is replaced. Development has moved to the ProMicro board.
