#!/usr/bin/env bash
# Produce a UF2 file that is flashable on ProMicro nRF52840 (Linux/macOS).
#
# WHY THIS SCRIPT IS NEEDED (cannot just flash zephyr.uf2 directly):
#   The bootloader is MCUboot with signature verification. The file
#   "build/<app>/zephyr/zephyr.uf2" is UNSIGNED -> MCUboot rejects it
#   -> the board never boots the app (symptoms: no app COM port, no
#   BLE advertisement, no log because the bootloader's console is
#   disabled).
#   You must merge: mcuboot.hex + the SIGNED app (zephyr.signed.hex)
#   -> then run uf2conv.
#
# Usage: ./make_uf2.sh      (run after a successful `west build`)
#
# Requires ZEPHYR_BASE and the Zephyr Python venv (with `intelhex`) on PATH.
# Typical: `source ~/zephyrproject/.venv/bin/activate`.

set -euo pipefail

if [[ -z "${ZEPHYR_BASE:-}" ]]; then
    echo "ZEPHYR_BASE not set. Activate your Zephyr environment before running this script." >&2
    exit 1
fi

PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FAMILY="0xADA52840"                              # nRF52840 - Adafruit UF2 family ID
OUT_UF2="${PROJ}/tag_ProMicro_SIGNED.uf2"
UF2CONV="${ZEPHYR_BASE}/scripts/build/uf2conv.py"

APP_DOMAIN="$(basename "${PROJ}")"
APP_HEX="${PROJ}/build/${APP_DOMAIN}/zephyr/zephyr.signed.hex"
[[ -f "${APP_HEX}" ]] || APP_HEX="${PROJ}/build/beacon/zephyr/zephyr.signed.hex"
MCU_HEX="${PROJ}/build/mcuboot/zephyr/zephyr.hex"
MERGED="${PROJ}/build/merged.hex"

for f in "${MCU_HEX}" "${APP_HEX}"; do
    [[ -f "${f}" ]] || { echo "Not found: ${f}  (did you run 'west build' first?)" >&2; exit 1; }
done

python3 - <<PY
from intelhex import IntelHex
mb  = IntelHex(r"${MCU_HEX}")
app = IntelHex(r"${APP_HEX}")
if mb.maxaddr() >= app.minaddr():
    raise SystemExit("ERROR: mcuboot and app addresses OVERLAP!")
m = IntelHex(); m.merge(mb, overlap="replace"); m.merge(app, overlap="replace")
m.write_hex_file(r"${MERGED}")
print("  mcuboot       0x%05x - 0x%05x  (expected to start at 0x26000)" % (mb.minaddr(), mb.maxaddr()))
print("  app (signed)  0x%05x - 0x%05x  (expected to start at 0x32000)" % (app.minaddr(), app.maxaddr()))
PY

python3 "${UF2CONV}" -c -f "${FAMILY}" -o "${OUT_UF2}" "${MERGED}"

size=$(stat -c%s "${OUT_UF2}" 2>/dev/null || stat -f%z "${OUT_UF2}")
echo ""
echo "OK -> ${OUT_UF2}  (${size} bytes)"
echo ""
echo "Flash: short RST to GND twice quickly to enter the bootloader, then copy the file above onto the mass-storage drive (mounts under /media/\$USER/ on Linux, /Volumes/ on macOS)."
