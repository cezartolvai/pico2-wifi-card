#!/bin/bash
# NAT-pressure soak for the CDC-NCM bridge.
#
# BRIDGE_NAT_TABLE_SIZE is 24 (src/config.h), so this opens far more concurrent
# flows than the table can hold.  That is the only way to reach the nat_snat()
# failure path in bridge_nat_output() from ordinary host traffic, which is the
# path that used to double-free the forwarded pbuf (see .clinerules/02-coding-standards.md,
# "Double-free in the bridge path").
#
# Run alongside a debug-console watcher:
#   python3 -u scripts/console_watch.py /dev/ttyACM1 240 > /tmp/soak.log &
#   bash scripts/nat_pressure.sh
# Expect: no "RECOVERED from", no assert, faults=0 and hangs=0 in `stats`.
#
# Override the interface with:  IF=enx02ce50d8db52 bash scripts/nat_pressure.sh
IF="${IF:-enx02ce50d8db52}"
WAVES="${WAVES:-3}"

for r in $(seq 1 "$WAVES"); do
    for n in $(seq 1 40); do
        ping -c 30 -i 0.2 -W 1 -I "$IF" "10.77.$((n * 3)).$((n + 40))" >/dev/null 2>&1 &
    done
    wait

    for n in $(seq 1 30); do
        curl -s -m 8 -o /dev/null "http://10.99.$n.1/" >/dev/null 2>&1 &
    done
    for n in $(seq 31 55); do
        timeout 6 bash -c "echo x | nc -u -w1 10.88.$n.1" >/dev/null 2>&1 &
    done
    wait

    echo "wave $r done"
done

echo "soak complete: $WAVES waves x (40 ICMP + 30 TCP + 25 UDP) on $IF"
