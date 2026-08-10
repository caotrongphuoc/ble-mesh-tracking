# tools/

Developer helper scripts. Run from anywhere — each script `cd`s to the repo root before it does anything.

## `format.sh`

Reformats every `.c` and `.h` under `apps/` in place, using the root [`.clang-format`](../.clang-format) (Allman, tabs, `ColumnLimit: 0`). Requires `clang-format` on PATH.

```
tools/format.sh
```

The [CONTRIBUTING guide](../.github/CONTRIBUTING.md) requires this to be clean before you open a PR.

## `build-all.sh`

Builds all four ESP-IDF apps (`gateway`, `relay`, `scanner`, `tag`) sequentially. Requires ESP-IDF to be sourced first:

```
. $IDF_PATH/export.sh
tools/build-all.sh
```

Each successful build drops its `.bin` into `firmware/` via the CMake `POST_BUILD` step — that is what the nginx OTA server serves. Override the drop location with `idf.py -DBMT_OTA_DIR=/other/dir build`.

## Not covered

- Beacon nRF52840 apps use Zephyr / west and are outside this script — build them by hand per [apps/Beacon_ProMicroNrf52840/README.md](../apps/Beacon_ProMicroNrf52840/README.md) and [apps/Beacon_XiaoNrf52840/README.md](../apps/Beacon_XiaoNrf52840/README.md).
- No script for signing / OTA packaging — the CMake POST_BUILD step already handles the ESP-IDF signed image; MCUboot signing for Beacon is documented in the per-board READMEs.
