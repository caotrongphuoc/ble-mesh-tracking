# tests/

Test skeleton for the algorithmic logic used by the firmware. These are **reference tests in Python** that run on any host machine without ESP-IDF — they encode the expected behavior of the two algorithms most likely to silently drift (HMAC-16 beacon signing and the adaptive-R Kalman filter) as golden vectors, so a future refactor can catch regressions.

Full on-target Unity tests are not wired up yet. Add them under an ESP-IDF test app when doing so.

## Run

Requires Python 3.8+.

```
python3 -m pytest tests/ -v
```

## What's covered

| File | Tests | What it locks down |
|---|---|---|
| [`test_hmac16_reference.py`](test_hmac16_reference.py) | HMAC-16 truncation, empty input, deterministic output for fixed key | The 16-bit truncation strategy and the byte order the firmware uses. If someone changes `bmt_auth_hmac16` to take the last 2 bytes instead of the first 2, or byte-swaps, these tests break. |
| [`test_kalman_reference.py`](test_kalman_reference.py) | Convergence on a constant signal, outlier resistance from adaptive R, K stays inside `[0, 1)` | The exact filter behavior documented in [`docs/03-algorithms.md`](../docs/03-algorithms.md). If the constants `q=0.1`, `r_alpha=0.1`, `r_min=1.0`, `r_max=20.0` are changed, expected values shift and the tests must be re-baselined. |

## Not covered yet (help wanted)

- Path-loss distance formula (`docs/03-algorithms.md#2-distance-from-rssi`).
- Anti-replay sequence-window check in `bmt_tag_table_update`.
- Hysteresis + leaky-bucket debounce logic in the ThingsBoard rule chain JS (unit-test the JS with Node + a small harness).
- Watchdog reset flow (harder — needs a mock mesh + timing).

## How to add an on-target Unity test

ESP-IDF ships Unity plus a CMock-lite. The idiomatic pattern is to create a test component next to the code:

```
apps/scanner/components/bmt_tag_table/
  bmt_tag_table.c
  bmt_tag_table.h
  CMakeLists.txt
  test/                       <-- add this
    CMakeLists.txt
    test_bmt_tag_table.c
```

`test/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "test_bmt_tag_table.c"
    INCLUDE_DIRS "."
    REQUIRES unity bmt_tag_table
)
```

Then run via ESP-IDF's `unity-test-app`:

```
cd $IDF_PATH/tools/unit-test-app
idf.py set-target esp32
idf.py -T bmt_tag_table build flash monitor
```

See the [ESP-IDF Unit Testing guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/unit-tests.html).

When the on-target tests exist, port the golden vectors from the Python files here so both layers pin the same expected values.
