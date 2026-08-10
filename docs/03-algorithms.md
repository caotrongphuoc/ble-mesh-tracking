# Algorithms

Numeric tricks used by firmware and rule chain. Each has a code pointer.

## 1. Kalman filter for RSSI

Where: `apps/scanner/components/bmt_tag_table/bmt_tag_table.c`.

Raw RSSI is noisy. We smooth with a 1D Kalman filter. `q = 0.1` is a fixed process-noise constant. `r` is **adaptive**: seeded at `2.0`, updated each sample by an EMA of the measurement innovation squared (`BMT_KALMAN_R_ALPHA = 0.1`), clamped to `[BMT_KALMAN_R_MIN=1.0, BMT_KALMAN_R_MAX=20.0]`.

Per sample:

```
innovation = rssi - x
r_var = (1 - alpha) * r_var + alpha * innovation^2
r     = clamp(r_var, R_MIN, R_MAX)
p     = p + q
k     = p / (p + r)
x     = x + k * innovation
p     = (1 - k) * p
```

`x` is the smoothed value we send over mesh. Adaptive `r` filters harder when RSSI gets noisy (multipath, someone blocking the tag) and tracks new samples faster when RSSI is stable — no manual retuning per environment. The clamps prevent `k` from collapsing to 0 (frozen) or shooting to 1 (unfiltered) on a single outlier.

## 2. Distance from RSSI

Where: same file, `calculate_distance()`.

BLE ADV includes `tx_power` (the RSSI at 1 m). Log-distance path loss:

```
distance = 10 ^ ((tx_power - rssi_filtered) / (10 * n))
```

`n` is the path-loss exponent. We use `BMT_PATH_LOSS_N = 2.5f` (typical indoor with walls). Result is noisy — use it for "close" vs "far room", not precise distance.

## 3. Anti-replay via sequence number

Where: `bmt_tag_table_update()`.

Each tag ADV carries a 1-byte sequence that increments every 500 ms. On a new packet:

- Same seq — duplicate. Log and skip.
- `diff <= -10` or `diff > BMT_MAX_SEQ_JUMP (30)` — tag reboot or replay attack. Reset filter, log warn.
- Small backward (1..9) — late packet from advertising overlap. Drop.
- Forward but skipped — count missed for loss stats.

The forward jump limit stops replay of an ADV captured hours ago.

## 4. HMAC-16 for beacon authentication

Where: `apps/tag/components/bmt_auth/bmt_auth.c` (build), `apps/scanner/components/bmt_auth/bmt_auth.c` (verify).

Tag ADVs include a 2-byte HMAC over the payload. Steps:

1. Build 24-byte payload with `mac16 = 0`.
2. `HMAC-SHA256(epoch_key, payload_without_mac16)`.
3. Take first 2 bytes.

Why only 2 bytes: ADV field is small. Combined with anti-replay and epoch derivation, it works well in practice.

Two independent HMAC key schedules are used, one for tag beacons and one for the OTA-trigger beacon. If one leaks, the other still works.

### 4a. Tag key: TOTP-style epoch derivation

The tag key is **not** a single static value shared by all boards. Both Tag and Scanner hold a shared 16-byte **master key** (hardcoded, identical byte-for-byte on both firmware). The actual `epoch_key` used to sign every ADV is derived from the master:

```
epoch_counter = esp_timer_get_time() / 1_hour
epoch_key     = HKDF(master_key, epoch_counter)
```

- The tag has no RTC. `esp_timer_get_time()` is a local monotonic uptime counter starting at 0 on every boot. Epochs are "hours since this tag was powered on", not wall-clock time.
- The scanner cannot know the tag's local uptime. On first successful ADV it "locks" onto whatever epoch the tag currently claims (`locked_epoch`), and thereafter accepts a narrow window around `locked_epoch` (drift tolerance for tick rounding).
- Result: an ADV captured in one epoch cannot be replayed in the next. An attacker who cannot compute HKDF (does not have `master_key`) cannot forge a valid epoch key either.

Tuning: epoch length is 1 hour by default. Shorter epochs shrink the replay window at the cost of a bigger drift-tolerance ratio.

### 4b. OTA-beacon key: plain rotation

Where: `apps/gateway/components/bmt_ota/bmt_ota.c`, `beacon_key_rotate_and_push()`.

The OTA-beacon key is a plain shared secret between gateway and all scanners. It rotates on wall-clock time on the gateway side:

Every 24 hours:

1. Generate fresh 16-byte key via `esp_fill_random()`.
2. Import to PSA. If fails, abort and keep old key.
3. Persist to NVS.
4. Push to every scanner via `OTA_KEY_PUSH` (encrypted by mesh AppKey).

Scanners import to their PSA slot and save to NVS. An attacker replaying a valid OTA beacon >24 h later gets rejected.

The OTA-beacon key does not use epoch derivation because the gateway has the whole mesh path to push a new one out, so plain rotate + push is simpler and gives the same guarantee.

## 6. Hysteresis for zone assignment

Where: rule chain node `Apply hysteresis` in `thingsboard/rulechain/ble_tag_zone_detection.json`.

To switch zone, the new best scanner must beat the current one by at least `HYSTERESIS_DBM = 5` dBm. Otherwise stay. Raise to 8-12 dBm if you see jitter; lower it if switching feels too slow.

## 7. Leaky-bucket debounce

Where: same rule chain node.

Require sustained evidence, not one outlier:

```
if new_zone == candidate_zone: candidate_count += 1
else: candidate_zone = new_zone; candidate_count = 1
if candidate_count >= DEBOUNCE_COUNT (2): commit switch
```

Leaky part: a "stay" report drops the counter by 1 instead of resetting to 0, tolerating one flap in a two-report window.

## 8. Fresh-sample window

Where: rule chain, `SCANNER_VALID_MS = 10000`.

Rule chain only considers RSSI samples <10 s old. A dead scanner's last value cannot keep influencing the zone.

## 9. Out-of-range timeout (gateway safety net)

Where: `bmt_thingsboard.c`, `zone_timeout_task`.

Rule chain fires only on new telemetry. If a tag disappears entirely, dashboard would freeze. Gateway task checks every 1 s: no telemetry in `BMT_TAG_OUT_OF_RANGE_MS = 10000` ms means publish `current_zone = "out_of_range"`.

## 10. OTA version compare

Where: `apps/*/CMakeLists.txt` (build side), `bmt_ota.c` (compare side).

`PROJECT_VER` is computed at build time as the newest source mtime, formatted `YYYYMMDDHHMMSS`. Because the format is fixed-width, `strncmp` on version strings equals chronological compare.

```
if (strncmp(new_desc.version, cur_desc->version, 14) <= 0) skip;   // not newer
```

## 11. SHA256 skip

Where: same file.

Every `.bin` embeds a SHA256. Skip flashing if identical:

```
if (memcmp(new_desc.app_elf_sha256, cur_desc->app_elf_sha256, 32) == 0) skip;
```

Runs before version compare. Prevents pointless flash wear on the 3-minute auto-check.

## 12. Data watchdog

Where: `apps/gateway/components/bmt_watchdog/bmt_watchdog.c`.

Protects against "mesh looks up but no data flows":

- 15 s stabilize after boot.
- After at least one scanner is configured, repeatedly snapshot
  `s_mesh_received`, sleep 30 s, and compare. Unchanged = mesh dead; changed =
  begin the next monitoring window.
- Broadcast `RESET_CMD` x5 with 1.5 s gap.
- Any send succeeded: wait 12 s, wipe, reboot.
- All 5 sends failed: retry loop (radio might have been busy).

Counter increments on `TAG_STATUS` AND ping ACKs, so "alive" is not tied only to tag traffic.

## 13. Factory reset button

Where: `apps/{gateway,scanner,relay}/components/bmt_factory_reset/bmt_factory_reset.c`.

Manual escape hatch for a mesh that got so stuck that watchdog cannot help (bad NVS, wrong keys, half-committed provision, etc.).

- A background task polls the BOOT button (GPIO0) every 100 ms.
- Any release before 10 s cumulative resets the counter.
- Hold continuously for 10 s -> erase every NVS namespace this app uses (mesh keys, node table, auth epoch state, OTA pending flag) and reboot.
- Firmware image itself is untouched: on reboot the node comes back unprovisioned. The gateway then re-provisions it as if it were a brand-new board.

Only the three network-participant apps have this. Tag does not, because a tag has no NVS state to wipe (its keys are compile-time constants).

## Where to tune

| Constant | File | Effect |
|---|---|---|
| `BMT_PATH_LOSS_N` | scanner `bmt_tag_table.h` | Distance slope. |
| Kalman `q`, `r` | scanner `bmt_tag_table.c` | Smoothing aggressiveness. |
| `BMT_MAX_SEQ_JUMP` | scanner `bmt_tag_table.h` | Anti-replay window. |
| `HYSTERESIS_DBM` | rule chain `Apply hysteresis` | Zone switch guard. |
| `DEBOUNCE_COUNT` | same | Consecutive-agreement requirement. |
| `SCANNER_VALID_MS` | same | Fresh-sample cutoff. |
| `BMT_TAG_OUT_OF_RANGE_MS` | gateway `bmt_zone.h` | OOR timeout. |
| `BMT_WDG_TIMEOUT_MS` | gateway `bmt_watchdog.c` | Watchdog window. |

Runtime behavior: [operation.md](05-operation.md). Protocol side: [ble-mesh.md](02-ble-mesh.md).
