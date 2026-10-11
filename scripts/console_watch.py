#!/usr/bin/env python3
"""Watch a CDC-ACM console through a crash loop.

The board may reset about every second, so opening the port once is not enough:
this waits for the device node to appear, dumps everything it emits until the
port disappears, then waits for it to come back.  It is how a boot banner and a
`RECOVERED from ...` line get caught even when the firmware does not stay up.

Usage:
    scripts/console_watch.py [device] [seconds]
"""
import os
import sys
import time

try:
    import serial
except ImportError:
    print("pyserial is missing: sudo apt install python3-serial", file=sys.stderr)
    raise SystemExit(2)

device = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
budget = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
commands = sys.argv[3:]

print(f"--- watching {device} for {int(budget)} s ---")
if commands:
    print(f"--- sending {commands} on every open ---")

end = time.time() + budget
cycles = 0

while time.time() < end:
    if not os.path.exists(device):
        time.sleep(0.1)
        continue

    try:
        ser = serial.Serial(port=device, baudrate=115200, timeout=0.2)
    except Exception as exc:
        print(f"open failed: {exc}")
        time.sleep(0.2)
        continue

    cycles += 1
    opened = time.time()
    total = 0

    # Send first: on a board that resets every second the command has to be in
    # flight before the poll loop gets a chance to die.
    for cmd in commands:
        try:
            ser.write((cmd + "\r\n").encode())
            ser.flush()
        except Exception:
            break

    while time.time() < end:
        try:
            pending = ser.in_waiting
        except Exception:
            break                      # the port died with the board
        if pending:
            data = ser.read(pending)
            total += len(data)
            print(f"[{time.time() - opened:6.2f}] "
                  f"{data.decode('utf-8', 'replace')}", end="")
            if not data.endswith(b"\n"):
                print()
        else:
            time.sleep(0.05)
        if not os.path.exists(device):
            break                      # the board reset

    print(f"[cycle {cycles}] {total} bytes before the port went away")
    try:
        ser.close()
    except Exception:
        pass
    time.sleep(0.3)
