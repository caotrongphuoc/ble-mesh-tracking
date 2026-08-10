# Beacon — ProMicro nRF52840 (nice!nano v2 compatible)

BLE beacon tag firmware for the **ProMicro nRF52840** board (e.g. the
"Nologo ProMicro nRF52840" from hshop) — compatible with the
nice!nano v2.

Sister app: [`../Beacon_XiaoNrf52840`](../Beacon_XiaoNrf52840) for
the XIAO nRF52840. **The beacon and auth logic is identical
byte-for-byte**; only the hardware-dependent parts differ (partition
layout, battery-read path, power configuration).

---

## 0. Overview

An **optional coin-cell variant** of [`../tag`](../tag) (ESP-IDF).
Same wire protocol byte-for-byte (CID `0x02E5`, 24-byte payload,
HMAC-16, TOTP-style epoch key) so ESP32 scanners recognise it out of
the box — no scanner-side change needed. Pick this variant when a
wearable coin-cell tag matters more than reflash-over-USB convenience;
stick with `apps/tag` on ESP32-S3 otherwise.

Build system is **Zephyr / west** (not ESP-IDF); see the
[Zephyr getting-started guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html).
The bootloader is **MCUboot** with ECDSA-P256 signing — the signing
key lives in `../../keys/` (see
[`../../keys/README.md`](../../keys/README.md) for the
regen command; the repo ships none).

For interop the master HMAC key must match byte-for-byte across every
tag variant and the Scanner — see
[`../../.github/SECURITY.md`](../../.github/SECURITY.md).

**Status: experimental.** No OTA-over-mesh support (the OTA path in
`apps/tag` is not shared with this variant); flashing is manual via
UF2 as described below.

---

## 1. Build

```powershell
west build -b promicro_nrf52840/nrf52840/uf2 -d build . --pristine
```

> The board target MUST end in **`/uf2`**. Only that variant uses the
> partition layout expected by the Adafruit UF2 bootloader
> (`nrf52840_partition_uf2_sdv6.dtsi`).

### The production build has NO COM port — INTENTIONAL

By default USB + console + log are **stripped out** because together
they draw ~0.8 mA continuously (nearly half of the total current) even
after the USB cable is unplugged — see section 4.

Consequence after flashing: **no COM port, no log**. This is correct,
not a bug. To confirm the tag is alive, watch the **Scanner** log
(`BMT_TAGTBL: New tag: 0x0001`) or scan with **RNF Digital Innovation
Beacon Toolkit** on iOS and look for `Test beacon`.

### When you need logs for debug

```powershell
west build -b promicro_nrf52840/nrf52840/uf2 -d build_debug . --pristine `
    -- "-DBeacon_ProMicroNrf52840_EXTRA_CONF_FILE=debug.conf"
```

`debug.conf` re-enables the USB CDC-ACM console and logs. **Do NOT
measure current on this build** — it will be ~0.8 mA higher than the
real firmware.

> The `Beacon_ProMicroNrf52840_` prefix is required: this is a
> multi-image sysbuild; without the prefix CMake configure fails with
> "CMake configure failed for Zephyr project".

---

## 2. Flash — READ THIS SECTION CAREFULLY

The bootloader is **MCUboot** with ECDSA-P256 signature verification.
This means **you cannot flash the app's raw `zephyr.uf2`**: that file
is **UNSIGNED**; MCUboot rejects it and the board never boots the app
(symptom: the UF2 drive disappears normally after copy, but
**no app COM port appears**, no BLE, no log — because `mcuboot.conf`
disables the bootloader's console).

> The XIAO variant lost hours to this same mistake: it looked like
> broken code or a broken board, but it was just an unsigned file.

### How to flash: merge MCUboot + SIGNED app, then convert to UF2

Run the helper script:

```powershell
.\make_uf2.ps1
```

It produces `tag_ProMicro_SIGNED.uf2`. Then:

1. Enter the bootloader: **short RST to GND twice quickly** (this
   board has no reset button — different from XIAO).
2. The USB drive appears -> copy the `.uf2` onto it.
3. The drive disappears on its own = the board reset and is now
   running the firmware.

### Sanity-check the file before flashing

| Check | Correct | Wrong |
|---|---|---|
| File size | ~321 KB (merged) | ~270 KB (app only, unsigned) |
| First data block address | `0x26000` (MCUboot) | `0x32000` (app) |

---

## 3. Differences vs the XIAO variant

| | XIAO nRF52840 | ProMicro nRF52840 |
|---|---|---|
| Adafruit bootloader | SoftDevice s140 **v7** | SoftDevice s140 **v6** |
| MCUboot placed at | `0x27000` | **`0x26000`** |
| App (slot0) at | `0x33000` | **`0x32000`** |
| Enter bootloader | double-tap RESET button | **short RST-GND twice** |
| Power path | battery -> BQ25101 -> external LDO -> VDD | **battery -> straight into VDDH** |
| Battery-voltage read | external 1M/510k divider -> P0.31, P0.14 must be LOW | **internal VDDHDIV5**, no GPIO needed |
| Charging status | read P0.17 | **none** (`is_charging()` always false) |
| DC/DC | board enables both (reg0 + reg1) | **not enabled** — see `prj.conf` |

Full details and reasoning: see the comments in
`boards/promicro_nrf52840_nrf52840_uf2.overlay`, `src/bmt_battery.c`,
and `prj.conf`.

---

## 4. Current-draw measurement (as of 2026-07-25)

Measurement setup: battery on the B+ pin, **USB unplugged**, DT-9205A
on the 200 mA DC range (0.1 mA resolution), meter in series on the
B+ rail.

> **ALWAYS UNPLUG USB BEFORE PUTTING A METER ON THE BATTERY RAIL.**
> Measuring current while charging destroyed the charger IC on the
> XIAO board — see `../Beacon_XiaoNrf52840/README.md`.

### Elimination log (each row = one build + flash + remeasure)

| # | Config | Reading | Conclusion |
|---|---|---|---|
| 1 | Initial (USB + log on) | 1.9 mA | ~100x above theory |
| 2 | Bigger USB buffers (`UDC_BUF_*`) | 1.9 mA | Fixes `net_buf` errors, no current change |
| 3 | `CONFIG_LOG=n` | 1.9 mA | Disabling log does NOTHING — only stops printing |
| 4 | Disable uart0 / i2c0 / i2c1 / spi2 | 1.8 mA | Floating pins are not the culprit |
| 5 | Turn off external VCC pin (P0.13 LOW) | 1.7 mA | Helps, but only 0.1 mA |
| 6 | **EMPTY** firmware (no BLE / ADC) | 1.0 mA | Remainder is hardware + USB |
| 7 | Empty firmware + P0.13 LOW | 0.9 mA | **Hardware floor** |
| 8 | **Production: strip USB, keep BLE + ADC** | **0.9 mA** | ⬅ **Final result** |

**Cut from 1.9 mA -> 0.9 mA (53%).**

### Reading the numbers

Rows #7 and #8 are **equal** — that is the key evidence: the full
firmware (BLE advertising at 1 s + battery reading) draws **under
the meter's resolution** (0.1 mA), consistent with the Nordic Online
Power Profiler estimate (~12 uA at 1000 ms interval, TX -4 dBm).
Meaning: **the firmware side is optimised as far as it can be**.

The remaining 0.9 mA is **on-board leakage of the clone board** —
firmware cannot touch it (evidence: row #7, empty firmware still
reads 0.9 mA).

### The most important lesson: turning off "log" does not turn off USB

The biggest saving (**~0.8 mA, nearly half of the total**) is
dropping the USB stack. But it has to be done via the RIGHT switch:

- `CONFIG_LOG=n` -> **useless**, only mutes prints; the USB stack
  keeps running.
- `CONFIG_CONSOLE=n` + `SERIAL=n` -> **still not enough**: `.config`
  still has `USB_DEVICE_STACK_NEXT=y` + `UDC_NRF=y`, and
  `CDC_ACM_SERIAL_ENABLE_AT_BOOT` defaults to `y`, so USB is
  enabled at boot even though the app never called `usb_enable()`.
- Correct: **`CONFIG_BOARD_SERIAL_BACKEND_CDC_ACM=n`** — the
  official switch; the whole cluster drops with it. See `prj.conf`.

### Battery-life estimate (at 0.9 mA)

| Battery | Capacity | Runtime |
|---|---|---|
| LiPo 1000 mAh | 1000 mAh | ~46 days |
| LiPo 500 mAh | 500 mAh | ~23 days |
| LIR2032 | ~40 mAh | ~2 days |

If the hardware leak is fixed (down to ~15-20 uA), a 500 mAh LiPo
would run for **~3 years** — in practice limited by the cell's
self-discharge rather than the circuit.

### Where the 0.9 mA comes from and why NOT to fix it

Per the reverse-engineered notes for this board
([sasodoma/nrf52840-promicro](https://github.com/sasodoma/nrf52840-promicro)):
the power-path cluster is **NPQ2 (MOSFET) + NBD1 (diode) + NPR7**,
and diode **W5** should be a Schottky BAT60B but many clones fit an
ordinary silicon diode instead -> high reverse leakage. Fix: remove
those three components and bridge pins 2-3 of NPQ2.

**Decision: DO NOT fix.** The SMD parts are tiny, board-damage risk
is real (already lost one XIAO board to a hardware incident), and
0.9 mA already yields ~23 days on a 500 mAh LiPo — plenty for demos
and for gathering experimental data.

### Battery notes

- **DO NOT connect a CR primary cell (CR2032 / CR2477, etc.) to the
  B+ pin.** "CR" cells are primary lithium — **not rechargeable**;
  the B+ pin sits behind a charger, and USB power would push charge
  current into it -> risk of swelling, venting, fire. Use **LIR**
  cells for a rechargeable coin cell.
- Coin cells have **very high internal resistance** (tens of ohms) ->
  each BLE TX pulse drops the voltage briefly -> the ADC reads a
  false low -> **battery % jumps around wildly**. Observed with a
  LIR2032: went 100% -> 53% in ~10 minutes, then charging popped it
  back to 100% immediately (obviously not fully charged in seconds).
  Use a **LiPo cell** — ~0.1-0.3 ohm internal, no such artefact.

### Remaining (low priority)

- [ ] Remeasure on the **2 mA** range (1 uA resolution instead of
      0.1 mA) for a tighter number in reports — 0.9 mA fits inside
      that range.
- [ ] Verify that `bmt_battery.c` reads the real battery voltage
      correctly (compare against a DMM).
- [ ] ~~Try enabling DC/DC via devicetree~~ — **skipped**: at 0.9 mA
      the MCU is no longer the dominant consumer, enabling DC/DC
      cannot help; and ZMK community data on clone boards shows it
      can actually INCREASE current (see `prj.conf`).
