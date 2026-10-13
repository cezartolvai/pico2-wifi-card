#!/bin/bash
# persistence_probe.sh - testul complet de persistență a înregistrării de configurare.
#
# Secvența (`save` -> citire sector -> reboot -> citire -> watchdog -> citire) fusese
# rulată manual, pas cu pas; fiecare pas uitat lasă concluzia fără sprijin. Scriptul o
# rulează într-o singură trecere, cu judecăți scrise în loc de cifre goale.
#
# Are nevoie de un build cu `test flash` / `test peek` pe aceeași cale de date:
#   cmake -S . -B build-selftest-l2 -G Ninja -DPICO_SDK_PATH=$PWD/.pico-sdk \
#         -DBRIDGE_SELFTEST=1 -DBRIDGE_L2_BRIDGE=1
#   env UF2=build-selftest-l2/pico2_wifi_card.uf2 scripts/flash.sh --copy
#
# Rulează:  scripts/persistence_probe.sh [tty] [ssid]
#   tty    default /dev/ttyACM0
#   ssid   default persist-probe
#
# Un singur deschizător per port: toate comenzile merg pe ttyACM0.
#
# Ce judecă, și de ce fiecare contează:
#   - `test flash` citește sectorul prin XIP își recalculează CRC-ul, deci spune exact ce
#     ar conchide settings_load() la următorul boot.
#   - `reboot` și `test hang` sunt hoțurile: dacă unul dintre ele șterge recordul, cauza
#     nu e settings_save(), ci calea de reset.
#   - `live` face radioul activ (scan continuu, deci host-wake activ). O scriere care
#     supraviețuiește aici este o gardă verificată, nu o presupunere.

TTY="${1:-/dev/ttyACM0}"
SSID="${2:-persist-probe}"
PROBE="$(dirname "$0")/console_probe.py"

probe() { timeout 20 python3 "$PROBE" "$TTY" "$@" 2>&1; }
flash_line() { grep -E 'record @|profiles in flash'; }
saved_line() { grep -E 'saved to flash|save failed'; }

echo "=== persistență pe $TTY, profil de test: $SSID ==="

echo "[0] sectorul la boot, înainte de orice save"
probe 'test flash' | grep -E 'record @|profiles in flash'
sleep 1

echo "[1] save cu radioul neasociat"
probe "set ssid $SSID" | grep -E 'applying|profiles:|ssid:'
sleep 1
probe 'set pass probe-password' | grep -E 'applying|pass:' | head -2
sleep 1
probe save | grep -E 'saved to flash|save failed'
sleep 1
probe 'test flash' | grep -E 'record @|profiles in flash'
sleep 1
probe 'test peek 0x3f0000 32' | grep -E '\+0000|\+0016'
sleep 1

echo "[2] reboot cerut de la consolă"
probe reboot | grep -E 'rebooting'
sleep 12
probe 'test flash' | grep -E 'record @|profiles in flash'
sleep 1

echo "[3] save x3 în timpul unui scan live (radioul lucrează)"
probe 'set debug on' | grep -E 'debug:'
sleep 1
probe live | grep -cE 'dBm'
for i in 1 2 3; do
    probe save | grep -E 'saved to flash|save failed'
    sleep 3
done
probe back | grep -E 'profiles:'
probe 'test flash' | grep -E 'record @|profiles in flash'
sleep 1

echo "[4] blocare reală de watchdog, apoi sectorul"
probe 'test hang' | grep -E 'hanging'
sleep 16
probe 'test flash' | grep -E 'record @|profiles in flash'
sleep 1
probe stats | grep -E '^stats:'

echo "[5] ce raportează consola după toate hoțurile"
probe list | grep -E 'no profiles|profiles:|ssid:|status:'
probe restore | grep -E 'restored from flash|nothing saved'
echo "=== sfârșit: fiecare rând crc ok înseamnă că recordul a supraviețuit pasului din fața lui ==="
