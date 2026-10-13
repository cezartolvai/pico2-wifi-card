#!/bin/bash
# provision_and_probe.sh - reproducează exact fereastra în care a dispărut recordul.
#
# De ce există: defectul din 2026-10-13 s-a produs într-o singură fereastră netestată -
# `save` cu stația asociată și cu trafic real prin punte. Celelalte căi au fost infirmate
# pe board (reboot simplu, reset de watchdog, `save` cu radioul scanând). Acest script
# deschide fereastra rămasă și citește sectorul din flash după fiecare pas.
#
# Are nevoie de imaginea care are `test flash` pe aceeași cale de date ca puntea L2:
#   cmake -S . -B build-selftest-l2 -G Ninja -DPICO_SDK_PATH=$PWD/.pico-sdk \
#         -DBRIDGE_SELFTEST=1 -DBRIDGE_L2_BRIDGE=1
#   env UF2=build-selftest-l2/pico2_wifi_card.uf2 scripts/flash.sh --copy
#
# Rulează:  env BRIDGE_WIFI_SSID='nume-retea' BRIDGE_WIFI_PASSWORD='parola' \
#           bash scripts/provision_and_probe.sh [interfata] [fluxuri]
#
# Credențialele vin din mediu, nu din argumente: ajung doar pe consola boardului și nu
# se scriu nicăieri pe disc. Fără ele scriptul refuză, pentru că un profil gol produce
# exact semnătura unei punți defecte (`link=nonet`, `txdrop` în creștere).
#
# Un singur deschizător per port: toate comenzile merg pe CONSOLE.

set -u

cd "$(dirname "$0")/.." || exit 1

CONSOLE="${CONSOLE:-/dev/ttyACM0}"
SSID="${BRIDGE_WIFI_SSID:-}"
PASSWORD="${BRIDGE_WIFI_PASSWORD:-}"
FLUXURI="${2:-80}"

if [ -z "$SSID" ] || [ -z "$PASSWORD" ]; then
    echo "[!] nevoie de credențiale în mediu; ele nu se comit și nu se tipăresc:"
    echo "    env BRIDGE_WIFI_SSID='nume-retea' BRIDGE_WIFI_PASSWORD='parola' \\"
    echo "        bash scripts/provision_and_probe.sh"
    exit 2
fi

probe() { timeout 25 python3 scripts/console_probe.py "$CONSOLE" "$@" 2>/dev/null | tr -d '\r'; }
flash_line() { grep -aE 'record @|profiles in flash'; }
saved_line() { grep -aE 'saved to flash|save failed'; }

echo "=== 1. provisionare (SSID afișat, parola nu este afișată niciodată) ==="
probe "set ssid $SSID" | grep -aE 'applying|ssid:'
sleep 1
probe "set pass $PASSWORD" | grep -aE 'applying|pass:' | head -2
sleep 1
probe save | grep -aE 'saved to flash|save failed'
sleep 1
probe 'test flash' | grep -aE 'record @|profiles in flash'

echo "=== 2. așteaptă asocierea reală (fără ea, pașii de mai jos nu testează nimic) ==="
linked=no
for i in 1 2 3 4 4 4; do
    sleep 5
    if probe stats | grep -aq 'link=up'; then linked=yes; break; fi
done
probe stats | grep -a '^stats:'
if [ "$linked" = no ]; then
    echo "  FĂRĂ UPLINK: stația nu s-a asociat. Testul nu poate fi rulat - un `save`"
    echo "  fără uplink nu e fereastra care a pierdut recordul, și a fost deja infirmat."
    exit 1
fi

echo "=== 3. trafic real prin punte, cu save în mijlocul lui ==="
console_dump=$(probe list)
usb_mac=$(printf '%s' "$console_dump" | grep -aoE 'usb [0-9a-f:]{17}' | head -1 | cut -d' ' -f2)
IF="${1:-enx${usb_mac//:/}}"
if ! ip -br link show "$IF" >/dev/null 2>&1; then
    echo "  NU EXISTĂ pe host: $IF"; ip -br link; exit 1
fi
echo "  fluxuri fixate pe $IF: $FLUXURI"
bash scripts/pinned_flow.sh "$IF" "$FLUXURI" 2>&1 | tail -3
probe save | grep -aE 'saved to flash|save failed'
sleep 1
probe 'test flash' | grep -aE 'record @|profiles in flash'
probe stats | grep -a '^stats:'

echo "=== 2. după trafic: reboot, apoi sectorul din nou ==="
probe reboot | grep -aE 'rebooting'
sleep 12
probe 'test flash' | grep -aE 'record @|profiles in flash'
probe list | grep -aE 'profiles:|ssid:|status:'

echo "=== verdict ==="
echo "  'crc ok' la fiecare pas = recordul a supraviețuit ferestrei cu trafic, deci"
echo "  cauza dispariției din 2026-10-13 este în altă parte și rămâne de căutat."
echo "  'erased' sau 'crc bad' la pasul 1 sau 2 = traficul prin punte e cauza; garda"
echo "  din settings_save() e singura apărare, iar un record A/B ar face-o imposibilă."
