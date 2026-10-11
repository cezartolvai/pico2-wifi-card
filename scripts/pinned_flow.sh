#!/bin/bash
# pinned_flow.sh - exerciteaza calea de punte si tabelul NAT fara sa atinga
# NIC-ul prin fir al hostului.
#
# De ce exista: testul care scoate NIC-ul prin fir (nmcli device set ... managed no)
# face puntea singura ruta de default. Daca tabelul NAT se satureaza exact in acel
# moment, hostul - si orice agent care lucreaza de pe acest host - isi pierde
# legatura in timp ce ar trebui sa raporteze rezultatul. Testul de mai jos obtine
# aceleasi fluxuri prin punte prin SO_BINDTODEVICE (-I), lasand restul hostului pe fir.
#
# Ce face: trimite N fluxuri distincte, fixate pe interfata puntii, si citeste
# ocuparea tabelului NAT de pe consola. Un flux refuzat de NAT arata ca un flux
# care nu primeste niciun raspuns, deci succesul fiecarei pinguri este verificarea.
#
# Ruleaza:  scripts/pinned_flow.sh [interfata] [numar-fluxuri]
#
# Se poate opri din CMake cu -DBRIDGE_NAT_ENABLE=0; atunci nu exista tabel NAT.

set -u

# Consola si scripturile sunt relative la repo, nu la directorul curent.
cd "$(dirname "$0")/.." || exit 1

IF="${1:-enx02ce50d8db52}"
N="${2:-40}"
CONSOLE="${CONSOLE:-/dev/ttyACM0}"

if ! ip -br link show "$IF" >/dev/null 2>&1; then
    echo "interfata '$IF' nu exista; ruleaza cu numele corect al interfeței CDC-NCM" >&2
    exit 1
fi

echo "=== ierarhia rutelor inainte (firul trebuie sa ramana preferat) ==="
ip route show | grep default
echo "=== legatura proprie a hostului ==="
ip route get 1.1.1.1 | head -1

probe() {
    timeout 15 python3 scripts/console_probe.py "$CONSOLE" stats 2>&1 \
        | tr -d '\r' | grep -aoE 'nat: +[0-9]+/[0-9]+ flows|stats: .*' | tail -2
}

ok=0
fail=0
for n in $(seq 1 "$N"); do
    if timeout 5 ping -c 1 -W 2 -I "$IF" 1.1.1.1 >/dev/null 2>&1; then
        ok=$((ok + 1))
    else
        fail=$((fail + 1))
        printf "flux %3d  FAIL  (reusite pana acum: %d)\n" "$n" "$ok"
    fi
    if [ $((n % 10)) -eq 0 ]; then
        printf -- "--- dupa %d fluxuri pinned pe punte ---\n" "$n"
        probe
    fi
done

printf "=== fluxuri NOUE prin punte: %d reusite / %d esuate din %d ===\n" "$ok" "$fail" "$N"
probe

echo "=== interfata pe host ==="
ip -s link show "$IF" | grep -A1 "RX:" | head -2
echo "=== firul ramane neatins ==="
ip route show | grep default