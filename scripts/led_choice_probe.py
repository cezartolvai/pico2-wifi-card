#!/usr/bin/env python3
"""Prove which LED pattern the state machine *chose*, not only how it looks.

`led_probe.py` times a waveform. This answers the other half of the LED
requirement: does bridge_led_pattern_for() actually pick the pattern the state
calls for?  `double` (live scan) is the reachable case - a live scan needs no
Wi-Fi credentials - so it is the one pattern choice that can be validated
without credentials.  `solid` and `1hz` need an association or a saved profile;
`off` cannot be read from a host that is reading the console at all.

The onboard LED is a CYW43 GPIO and cannot be read back: cyw43_ll_gpio_get()
sends the "ccgpioin" ioctl, the radio's input state, not the "gpioout" value the
firmware wrote.  The readout below is therefore the chooser's own decision, as
reported by `test led`.

Needs an image built with -DBRIDGE_SELFTEST=1 (build-selftest).

Usage:
    scripts/led_choice_probe.py [device]

    scripts/led_choice_probe.py /dev/ttyACM0
"""
import re
import sys
import time

import serial

LED_RE = re.compile(r"led:\s+(\S+)\s+level\s+(\S+)\s+tick\s+(\d+)\s+state asks\s+(\S+)")


def drain(stream, seconds):
    """Collect everything the console says for `seconds`."""
    buf = bytearray()
    end = time.time() + seconds
    while time.time() < end:
        chunk = stream.read(4096)
        if chunk:
            buf += chunk
        else:
            time.sleep(0.05)
    return buf.decode("utf-8", "replace")


def send(stream, line):
    stream.write((line + "\r\n").encode())
    stream.flush()


def report(stream, label):
    """Ask the board what it is driving, and print that next to its own verdict."""
    send(stream, "test led")
    text = drain(stream, 2.5)
    match = LED_RE.search(text)
    if match is None:
        print("    [%-7s] no `test led:` readout came back" % label)
        return None

    driven = match.group(1)
    level = match.group(2)
    tick = int(match.group(3))
    asked = match.group(4)
    print("    [%-7s] driven=%-7s level=%-3s tick=%2d  state asks=%-6s  %s"
          % (label, driven, level, tick, asked,
             "match" if driven == asked else "MISMATCH"))
    return driven


def main(argv):
    port = argv[1] if len(argv) > 1 else "/dev/ttyACM0"

    try:
        stream = serial.Serial(port, 115200, timeout=0.5)
    except Exception as exc:
        print("cannot open %s: %s" % (port, exc), file=sys.stderr)
        return 2

    try:
        time.sleep(0.5)
        stream.reset_input_buffer()

        print("baseline (no scan running):")
        baseline = report(stream, "baseline")

        send(stream, "scan")
        time.sleep(1.0)
        drain(stream, 1.0)
        send(stream, "live")
        time.sleep(1.5)
        print("during a live scan:")
        live = report(stream, "live")

        send(stream, "back")
        time.sleep(1.0)
        print("after `back`:")
        after = report(stream, "back")

        live_ok = (live == "double")
        back_ok = (after is not None) and (after == baseline)
        print()
        print("choice of `double` during a live scan: %s"
              % ("confirmed" if live_ok else "NOT seen"))
        print("return to `%s` after `back`: %s"
              % (baseline, "confirmed" if back_ok else "NOT seen"))
        return 0 if (live_ok and back_ok) else 1
    except Exception as exc:
        print("probe failed: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
