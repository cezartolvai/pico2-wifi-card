#!/bin/sh
# flash.sh - put an image on the board, by the path that erases the least.
#
# Two paths exist and they are NOT equivalent:
#
# 1. picotool load -x  (preferred).  picotool "will ignore any E10 abs-blocks"
#    (pico-sdk#1943), so it does not erase the final 4 kB sector of the flash.
#    Needs a picotool built with USB support: the one the SDK builds in
#    build/_deps/picotool has none ("compiled without USB support", no `load`),
#    so PICOTOOL points at a real one.
# 2. Copying the .uf2 onto the BOOTSEL drive.  This is what a user does, and it
#    "will unnecessarily erase the last 4KiB of connected flash" on an RP2350
#    whatever the size of the image - which is why the settings record no longer
#    lives in that sector (SETTINGS_OFFS in src/console.c).
#
# Usage:
#   scripts/flash.sh                                  # build/pico2_wifi_card.uf2
#   env UF2=build-l2/pico2_wifi_card.uf2 scripts/flash.sh
#   scripts/flash.sh --copy                           force the BOOTSEL-copy path
#   env PICOTOOL=/usr/bin/picotool scripts/flash.sh
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
UF2=${UF2:-$ROOT/build/pico2_wifi_card.uf2}
PICOTOOL=${PICOTOOL:-/tmp/picotool-usb/picotool}
MODE=${1:-auto}

boot_drive() {
    drive=$(findmnt -rn -o TARGET 2>/dev/null | grep -E '/(RP2350|RPI-RP2)$' | head -1)
    if [ -z "$drive" ]; then
        for candidate in /media/*; do
            case "$candidate" in
                */RP2350|*/RPI-RP2) drive=$candidate; break ;;
            esac
        done
    fi
    echo "$drive"
}

wait_for_drive() {
    i=0
    while [ "$i" -lt 30 ]; do
        drive=$(boot_drive)
        if [ -n "$drive" ]; then
            echo "$drive"
            return 0
        fi
        i=$((i + 1))
        sleep 1
    done
    return 1
}

if [ ! -f "$UF2" ]; then
    echo "flash.sh: no image at $UF2" >&2
    exit 1
fi

drive=$(boot_drive)
if [ -z "$drive" ]; then
    echo "flash.sh: no BOOTSEL drive; asking the running firmware to reboot into BOOTSEL"
    if [ -e /dev/ttyACM0 ]; then
        python3 "$ROOT/scripts/console_probe.py" /dev/ttyACM0 "reboot bootsel" >/dev/null 2>&1
    fi
    drive=$(wait_for_drive)
fi
if [ -z "$drive" ]; then
    echo "flash.sh: no BOOTSEL drive.  Hold BOOTSEL while plugging the board in." >&2
    exit 1
fi
echo "flash.sh: BOOTSEL drive $drive"

if [ "$MODE" = "--copy" ]; then
    use_copy=1
elif "$PICOTOOL" help load >/dev/null 2>&1; then
    use_copy=0
else
    echo "flash.sh: $PICOTOOL has no 'load' (built without USB support); using the BOOTSEL copy"
    use_copy=1
fi

if [ "$use_copy" = 1 ]; then
    echo "flash.sh: copying $(basename "$UF2") to $drive  (this erases the last 4 kB sector)"
    cp "$UF2" "$drive/" || exit 1
    sync
    sync
else
    echo "flash.sh: $PICOTOOL load -v -u -x -t uf2 $UF2"
    "$PICOTOOL" load -v -u -x -t uf2 "$UF2" || exit 1
fi

# Wait for the board to come back as a device, not as a drive.
i=0
while [ "$i" -lt 30 ]; do
    if lsusb 2>/dev/null | grep -qi '2e8a'; then
        echo "flash.sh: board re-enumerated"
        lsusb 2>/dev/null | grep -i 2e8a
        ls /dev/ttyACM* 2>/dev/null
        exit 0
    fi
    i=$((i + 1))
    sleep 1
done
echo "flash.sh: flashed, but the board had not re-enumerated after 30 s" >&2
exit 1
