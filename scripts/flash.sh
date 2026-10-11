#!/usr/bin/env bash
# Flash build/pico2_wifi_card.uf2 through the BOOTSEL drive.
#
# build/_deps/picotool was compiled without USB support, so `picotool load` is
# not available here: the uf2 has to be copied onto the BOOTSEL drive.  This
# script waits for that drive, copies, then waits for the board to come back as
# an application device - one button press, one command.
#
#   scripts/flash.sh
#   UF2=build-usbonly/pico2_wifi_card.uf2 scripts/flash.sh
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$PATH"
set -euo pipefail
cd "$(dirname "$0")/.."

UF2="${UF2:-build/pico2_wifi_card.uf2}"
if [ ! -f "$UF2" ]; then
    echo "no such image: $UF2" >&2
    exit 1
fi
echo "image : $UF2 ($(stat -c %s "$UF2") bytes, md5 $(md5sum "$UF2" | cut -c1-12))"

find_boot_drive() {
    # RP2350 on RP2350-arm-s boards, RPI-RP2 on RP2040 boards.
    findmnt -rn -o TARGET 2>/dev/null | grep -E '/(RP2350|RPI-RP2)$' | head -1
}

drive=""
echo "waiting for the BOOTSEL drive (hold BOOTSEL, plug the board in)..."
for _ in $(seq 1 240); do
    drive="$(find_boot_drive || true)"
    [ -n "$drive" ] && break
    sleep 0.5
done
if [ -z "$drive" ]; then
    echo "no BOOTSEL drive appeared; hold BOOTSEL and reconnect the board" >&2
    exit 1
fi

echo "drive : $drive"
cp "$UF2" "$drive/"
sync
echo "copied; waiting for the board to reset and re-enumerate"

for _ in $(seq 1 60); do
    if lsusb 2>/dev/null | grep -qi '2e8a:4022'; then
        lsusb | grep -i '2e8a'
        echo "flashed: board is back as an application device"
        exit 0
    fi
    sleep 0.5
done

echo "copied, but 2e8a:4022 never came back" >&2
exit 1
