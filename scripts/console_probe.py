#!/usr/bin/env python3
"""Probe a CDC-ACM console on the bridge: send commands, capture what comes back.

Usage:
    scripts/console_probe.py [device] [command ...]

Default device is /dev/ttyACM0, the management console.  Whatever the board
sends back is printed as it arrives, so a silent console shows up as silence
instead of hiding behind a timeout.  Baud is irrelevant: CDC-ACM is USB.
"""
import sys
import time

try:
    import serial
except ImportError:
    print("pyserial is missing: sudo apt install python3-serial", file=sys.stderr)
    sys.exit(2)

device = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
commands = sys.argv[2:] or ["help"]

try:
    ser = serial.Serial(port=device, baudrate=115200, timeout=0.2)
except Exception as exc:
    print(f"cannot open {device}: {exc}", file=sys.stderr)
    sys.exit(1)

print(f"--- {device} ---")
ser.reset_input_buffer()

for cmd in commands:
    print(f">> {cmd}")
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    ser.write((cmd + "\r\n").encode())
    ser.flush()

    deadline = time.time() + 3.0
    got = 0
    while time.time() < deadline:
        pending = ser.in_waiting
        if pending:
            sys.stdout.write(ser.read(pending).decode("utf-8", "replace"))
            sys.stdout.flush()
            got += pending
            deadline = time.time() + 1.0     # keep reading while it keeps talking
        else:
            time.sleep(0.05)
    print(f"<< {got} bytes")

ser.close()
