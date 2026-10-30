# Produce a UF2 file that is flashable on the XIAO or ProMicro nRF52840.
# The board is read from the build tree.
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
# Usage: .\make_uf2.ps1      (run after a successful `west build`)

$ErrorActionPreference = "Stop"

# Requires the Zephyr / nRF Connect SDK environment to be activated first
# (so ZEPHYR_BASE is set and `python` + `intelhex` are on PATH). Typical:
#   & "$env:USERPROFILE\ncs\vX.Y.Z\zephyr\zephyr-env.ps1"
if (-not $env:ZEPHYR_BASE) {
    throw "ZEPHYR_BASE not set. Activate your Zephyr environment before running this script."
}

$Py        = "python"
$Uf2Conv   = "$env:ZEPHYR_BASE\scripts\build\uf2conv.py"
$Proj      = $PSScriptRoot
$Family    = "0xADA52840"                # nRF52840 - Adafruit UF2 family ID

$Cache = "$Proj\build\CMakeCache.txt"
if (-not (Test-Path $Cache)) { throw "Not found: $Cache  (did you run 'west build' first?)" }
$Board = (Select-String -Path $Cache -Pattern '^BOARD:[A-Z]*=(.*)$').Matches[0].Groups[1].Value

# Per board: UF2 name, expected MCUboot / app start (see boards/mcuboot_*.overlay),
# how to enter the bootloader.
if ($Board -like "xiao_ble*") {
    $OutUf2 = "$Proj\tag_Xiao_SIGNED.uf2"; $McuStart = "0x27000"; $AppStart = "0x33000"
    $HowToFlash = "double-tap the RESET button to enter the bootloader, then copy the file above onto the XIAO-SENSE drive."
} elseif ($Board -like "promicro_nrf52840*") {
    $OutUf2 = "$Proj\tag_ProMicro_SIGNED.uf2"; $McuStart = "0x26000"; $AppStart = "0x32000"
    $HowToFlash = "double-tap RST-GND to enter the bootloader, then copy the file above into the USB drive that appears."
} else {
    throw "Unsupported board '$Board' in $Cache"
}

# Domain folder under build/ = the project folder name (sysbuild names
# it that way).
$AppDomain = Split-Path $Proj -Leaf
$AppHex = "$Proj\build\$AppDomain\zephyr\zephyr.signed.hex"
$McuHex = "$Proj\build\mcuboot\zephyr\zephyr.hex"
$Merged = "$Proj\build\merged.hex"

foreach ($f in @($McuHex, $AppHex)) {
    if (-not (Test-Path $f)) { throw "Not found: $f  (did you run 'west build' first?)" }
}

# Merge the two flash regions and print each region for a sanity check.
& $Py -c @"
from intelhex import IntelHex
mb  = IntelHex(r'$McuHex')
app = IntelHex(r'$AppHex')
if mb.maxaddr() >= app.minaddr():
    raise SystemExit('ERROR: mcuboot and app addresses OVERLAP!')
m = IntelHex(); m.merge(mb, overlap='replace'); m.merge(app, overlap='replace')
m.write_hex_file(r'$Merged')
print('  mcuboot       0x%05x - 0x%05x  (expected to start at $McuStart)' % (mb.minaddr(), mb.maxaddr()))
print('  app (signed)  0x%05x - 0x%05x  (expected to start at $AppStart)' % (app.minaddr(), app.maxaddr()))
"@
if ($LASTEXITCODE -ne 0) { throw "hex merge failed" }

& $Py $Uf2Conv -c -f $Family -o $OutUf2 $Merged
if ($LASTEXITCODE -ne 0) { throw "uf2conv failed" }

$size = (Get-Item $OutUf2).Length
Write-Output ""
Write-Output "OK -> $OutUf2  ($size bytes)"
Write-Output ""
Write-Output "Flash ($Board): $HowToFlash"
