#!/bin/bash
# l2_validate.sh - criteriile de acceptare ale punții L2, într-o singură trecere.
#
# De ce există: criteriile sunt șase, citite din trei locuri diferite (consola de
# management, fluxul de depanare, hostul însuși), și fiecare trebuie să fie adevărat în
# ACEEAȘI sesiune. Rulate pe rând se citesc greșit: o interfață cu MAC-ul stației dar fără
# lease nu spune nimic despre punte, iar un ping care merge nu spune nimic despre MAC.
#
# Criteriile (README.md, „Validating the L2 bridge”):
#   1. MAC-ul interfeței USB == MAC-ul stației CYW43 (rândul `mac:` de pe consolă)
#   2. `wifi IP: (bridge: none)` - puntea nu deține adresă
#   3. `host IPv4:` = o adresă din plasa routerului, nu 172.28.0.x
#   4. niciun rând `nat:` în dump
#   5. `mode: transparent L2 bridge (no NAT, no lease)`
#   6. `stats:` cu ->wifi și ->host ambele în creștere
#
# Rulează:  scripts/l2_validate.sh [interfata] [fluxuri]
#   interfata  default: enx<mac-ul stației>, aflat de pe consola boardului
#   fluxuri   default: 80
#
# Nu atinge NIC-ul prin fir al hostului: fluxurile de test sunt fixate pe punte cu
# SO_BINDTODEVICE (-I), deci restul hostului rămâne pe ruta lui preferată. Fără asta, o
# sesiune de validare care taie legătura hostului nu mai poate fi raportată.

set -u

cd "$(dirname "$0")/.." || exit 1

CONSOLE="${CONSOLE:-/dev/ttyACM0}"
DEBUG_TTY="${DEBUG_TTY:-/dev/ttyACM1}"
FLUXURI="${2:-80}"
ok=0   # initialized: `ok=$((ok + 1))` under `set -u` on an unset ok is an error, and
        # the first successful ping would then fail to count itself.

# O singură citire a consolei, refolosită la toate criteriile. Un al doilea proces pe
# același port împarte inputul cu primul și face consola să pară mută.
console_dump=$(timeout 15 python3 scripts/console_probe.py "$CONSOLE" list 2>/dev/null \
              | tr -d '\r')

usb_mac=$(printf '%s' "$console_dump" | grep -aoE 'usb [0-9a-f:]{17}' | head -1 | cut -d' ' -f2)
station_mac=$(printf '%s' "$console_dump" | grep -aoE 'station [0-9a-f:]{17}' | head -1 | cut -d' ' -f2)

echo "=== 1. identitate: un singur MAC, end-to-end ==="
printf '%s\n' "$console_dump" | grep -a 'mac:'
if [ -z "$usb_mac" ] || [ -z "$station_mac" ]; then
    echo "  FĂRĂ CITIRE: consola nu a raportat rândul mac: (imagine veche pe board?)" >&2
    exit 1
fi

IF="${1:-enx${usb_mac//:/}}"
if ! ip -br link show "$IF" >/dev/null 2>&1; then
    echo "  NU EXISTĂ pe host: $IF. Kernelul vede:" >&2
    ip -br link >&2
    exit 1
fi
host_mac=$(ip -o link show "$IF" | grep -oE 'link/ether [0-9a-f:]+' | cut -d' ' -f2)
echo "  interfață: $IF  host $host_mac"
echo "  stație:    $station_mac"
if [ "$host_mac" = "$usb_mac" ] && [ "$host_mac" = "$station_mac" ]; then
    echo "  OK: hostul adoptă MAC-ul stației (R3)."
else
    echo "  EȘUEAZĂ: host $host_mac, stație $station_mac (R3 neîndeplinită)." >&2
fi

echo "=== ce raportează puntea despre sine ==="
printf '%s\n' "$console_dump" | grep -aE 'wifi IP|host IPv4|mode:|nat:'
if printf '%s' "$console_dump" | grep -qa 'nat:'; then
    echo "  EȘUEAZĂ: există un rând nat: - boardul nu rulează build-ul L2." >&2
else
    echo "  OK: niciun rând nat: (fără NAT, niciun tabel de fluxuri)."
fi
if printf '%s' "$console_dump" | grep -qE 'host IPv4: +172\.28\.0\.'; then
    echo "  EȘUEAZĂ: hostul are încă o adresă din plasa privată a Pico." >&2
fi

stats_line() {
    timeout 15 python3 scripts/console_probe.py "$CONSOLE" stats 2>&1 \
        | tr -d '\r' | grep -aoE 'stats: .*' | tail -1
}
field() { printf '%s' "$1" | grep -oE "$2=[0-9]+" | head -1 | cut -d= -f2; }

echo "=== adresa hostului: a cui este ==="
ip -br addr show "$IF"
nmcli -f IP4.ADDRESS,IP4.GATEWAY,DHCP4.OPTION device show "$IF" 2>/dev/null \
    | grep -aE 'IP4.ADDRESS|IP4.GATEWAY|server_id|domain_name' | head -6
lease=$(ip -4 -o addr show dev "$IF" scope global | awk '{print $4}' | head -1)
if [ -z "$lease" ]; then
    echo "  FĂRĂ LEASE pe interfața punții."
    echo "  Semnătură: radioul nu e asociat, deci routerul nu are cum să răspundă."
    echo "  Verifică pe consolă: status / link în stats: (nonet înseamnă SSID salvat dar absent)."
else
    echo "  lease: $lease"
    case "$lease" in
        172.28.0.*) echo "  EȘUEAZĂ: adresa vine din pool-ul Pico, nu de la router (R5)." >&2 ;;
        *) echo "  OK: hostul are o adresă care nu aparține Pico (R5, fără subrețea privată)." ;;
    esac
    router_gw=$(ip route show default | awk -v i="$IF" '$0 ~ i {print $3; exit}')
    echo "  gateway văzut de host: ${router_gw:-none}"
fi

echo "=== contoarele punții, în jurul unui val de fluxuri ==="
before=$(stats_line)
echo "  înainte: $before"

echo "  -> $FLUXURI fluxuri fixate pe punte (SO_BINDTODEVICE; NIC-ul prin fir nu e atins)"
for i in $(seq 1 "$FLUXURI"); do
    ping -c 1 -W 2 -I "$IF" 1.1.1.1 >/dev/null 2>&1 && ok=$((ok + 1))
done
during=$(stats_line)
after=$(stats_line)
echo "  înainte: $before"
echo "  în timpul valului: $during"
echo "  după:  $after"

wifi_before=$(field "$before" '\->wifi')
wifi_during=$(field "$during" '\->wifi')
wifi_after=$(field "$after" '\->wifi')
host_before=$(field "$before" '\->host')
host_during=$(field "$during" '\->host')
host_after=$(field "$after" '\->host')
echo "  ->wifi  $wifi_before -> $wifi_during -> $wifi_after   (host -> Wi-Fi)"
echo "  ->host  $host_before -> $host_during -> $host_after   (Wi-Fi -> host)"
echo "  wdrop $(field "$after" 'wdrop')  rxdrop $(field "$after" 'rxdrop')  refl $(field "$after" 'refl')  poolfail $(field "$after" 'poolfail')"

if [ "${ok:-0}" -gt 0 ]; then
    echo "  OK: $ok din $FLUXURI fluxuri au primit răspuns prin punte."
else
    echo "  EȘUEAZĂ: niciun flux nu a trecut. Fără asociere, puntea nu are ce bridga." >&2
fi

echo "  IPv6 prin punte (criteriul R6):"
if ip -6 -br addr show "$IF" | grep -qE 'fe80|g'; then
    if ping -6 -c 2 -W 3 -I "$IF" 2606:4700:4700::1111 >/dev/null 2>&1; then
        echo "    OK: un ping IPv6 a trecut prin punte, fără ca Pico să vadă IPv6 (LWIP_IPV6=0)."
    else
        echo "    nu a trecut: IPv6 nu e validat pe acest AP."
    fi
else
    echo "    interfața n-are adresă IPv6; nu se poate verifica."
fi

echo "  consola de depanare ($DEBUG_TTY), 20 s de flux:"
# Write-only: consola de depanare nu are prompt, deci se citește cu watcher-ul, nu
# cu probe-ul (care se sincronizează după prompt și ar raporta consola ca mută).
#
# Si trebuie armata inainte. `debug: off` este starea de dupa un boot, deci o
# citire pe ttyACM1 fara `set debug on` intoarce 0 bytes si portul ramane deschis -
# identic cu semnatura unui board mort. Masurat 2026-10-13: "[cycle 1] 0 bytes
# watched to the end of the budget (the port stayed open)" inainte de armaare.
timeout 20 python3 scripts/console_probe.py "$CONSOLE" set debug on >/dev/null 2>&1
sleep 2
timeout 25 python3 scripts/console_watch.py "$DEBUG_TTY" 20 2>&1 | tail -8

echo "=== verdict ==="
echo "Criteriile de acceptare sunt în README.md, secțiunea „Validating the L2 bridge”."
echo "Fiecare rând de mai sus se raportează cu cifrele citite, nu cu așteptarea."

