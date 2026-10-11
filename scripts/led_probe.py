#!/usr/bin/env python3
"""Sample the bridge's own LED readout, and time the pattern it reports.

The onboard LED is a CYW43 GPIO and cannot be read back from the firmware:
cyw43_ll_gpio_get() issues the ioctl "ccgpioin", the radio's input state, not
the "gpioout" value bridge_set_led() wrote.  So a pattern is timed from the
readout a self-test build keeps about its own chooser: `test led` reports the
pattern the chooser picked, the level it drove, and its place in the ten-tick
cycle.

Needs an image built with -DBRIDGE_SELFTEST=1 (build-selftest).

Usage:
    scripts/led_probe.py [device] [seconds] [pattern [hold-seconds]]

    scripts/led_probe.py                              # what the state drives
    scripts/led_probe.py /dev/ttyACM0 6 solid 6       # hold "solid", time it
    scripts/led_probe.py /dev/ttyACM0 6 5hz 6         # hold the 5 Hz waveform
"""
import re
import sys
import time

try:
    import serial
except ImportError:
    print("pyserial is missing: sudo apt install python3-serial", file=sys.stderr)
    sys.exit(2)

device = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
budget = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0
arm = sys.argv[3:]

LED_RE = re.compile(r"led:\s+(\S+)\s+level\s+(\S+)\s+tick\s+(\d+)")

try:
    ser = serial.Serial(port=device, baudrate=115200, timeout=0.1)
except Exception as exc:
    print(f"cannot open {device}: {exc}", file=sys.stderr)
    sys.exit(1)

print(f"--- sampling {device} for {int(budget)} s ---")

try:
    if arm:
        line = "test led " + " ".join(arm)
        print(f">> {line}")
        ser.write((line + "\r\n").encode())
        ser.flush()
        time.sleep(0.3)
        ser.reset_input_buffer()

    samples = []
    end = time.time() + budget

    while time.time() < end:
        ser.write(b"test led\r\n")
        ser.flush()

        buf = ""
        match = None
        deadline = time.time() + 0.4
        while (match is None) and (time.time() < deadline):
            pending = ser.in_waiting
            if pending:
                buf += ser.read(pending).decode("utf-8", "replace")
                match = LED_RE.search(buf)
            else:
                time.sleep(0.01)

        if match is not None:
            samples.append((time.time(), match.group(1), match.group(2),
                            int(match.group(3))))

    print(f"sampled: {len(samples)} reads")
    if not samples:
        print("no `test led:` readout came back: is the board running build-selftest?")
        sys.exit(1)

    grouped = {}
    for stamp, pattern, level, tick in samples:
        grouped.setdefault(pattern, []).append((stamp, level))

    for pattern in sorted(grouped):
        rows = grouped[pattern]
        on = sum(1 for stamp, level in rows if level == "on")
        edges = sum(1 for i in range(1, len(rows)) if rows[i][1] != rows[i - 1][1])
        span = rows[-1][0] - rows[0][0]
        period = span / edges if edges else 0.0
        print(f"  {pattern:<7} reads {len(rows):4}  on {on:4}  off {len(rows) - on:4}"
              f"  edges {edges:4}  edge spacing {period:.3f} s")

    # The firmware advances one tick per BRIDGE_LED_TICK_MS; the tick numbers in
    # the samples say how fast that really is.  Only samples that landed on
    # different ticks contribute, which is what makes the estimate honest.
    ticks = 0
    for i in range(1, len(samples)):
        ticks += (samples[i][3] - samples[i - 1][3]) % 10

    span = samples[-1][0] - samples[0][0]
    if ticks and (span > 0.0):
        print(f"tick rate: {ticks / span:.2f} ticks/s "
              f"= {1000.0 * span / ticks:.0f} ms per tick")
except KeyboardInterrupt:
    pass
finally:
    try:
        ser.close()
    except Exception:
        pass
