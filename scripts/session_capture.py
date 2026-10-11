#!/usr/bin/env python3
"""Record the debug console while you provision the board by hand.

Why this exists: you provision over `picocom /dev/ttyACM0`, so nothing else should
hold that port.  This opens only the debug function (`/dev/ttyACM1`) and writes down
what the bridge reports while you type - association changes and every `stats:` line
- so the evidence of a real association is captured without competing with the
management console.

Usage:
    scripts/session_capture.py [seconds] [--mgmt TTY] [--debug TTY] [--arm]

    scripts/session_capture.py                 # 120 s on /dev/ttyACM1
    scripts/session_capture.py 180
    scripts/session_capture.py 180 --arm       # enable debug first, then let go
"""
import re
import sys
import time

import serial

EVENT_RE = re.compile(r"associat|link=up|wifi up", re.IGNORECASE)


def parse(argv):
    seconds = 120
    mgmt_tty = "/dev/ttyACM0"
    debug_tty = "/dev/ttyACM1"
    arm = False

    index = 1
    while index < len(argv):
        item = argv[index]
        if item == "--mgmt":
            mgmt_tty = argv[index + 1]
            index += 2
        elif item == "--debug":
            debug_tty = argv[index + 1]
            index += 2
        elif item == "--arm":
            arm = True
            index += 1
        else:
            seconds = int(item)
            index += 1

    return seconds, mgmt_tty, debug_tty, arm


def arm_debug(mgmt_tty):
    """Send `set debug on`, then release the management port immediately."""
    try:
        stream = serial.Serial(mgmt_tty, 115200, timeout=0.5)
    except Exception as exc:
        print("cannot open %s: %s" % (mgmt_tty, exc), file=sys.stderr)
        return False
    time.sleep(0.4)
    stream.write(b"set debug on\r\n")
    stream.flush()
    time.sleep(0.6)
    stream.close()
    print("enabled the debug stream on %s and released it" % mgmt_tty)


def capture(seconds, debug_tty):
    stream = serial.Serial(debug_tty, 115200, timeout=0.5)
    end = time.time() + seconds
    total = 0
    stats_lines = 0
    events = []

    while time.time() < end:
        chunk = stream.read(4096)
        if not chunk:
            time.sleep(0.05)
            continue
        for raw in chunk.decode("utf-8", "replace").splitlines():
            line = raw.strip()
            if not line:
                continue
            total += 1
            print("    " + line)
            if "stats:" in line:
                stats_lines += 1
            elif EVENT_RE.search(line):
                events.append(line)

    stream.close()
    return total, stats_lines, events


def main(argv):
    seconds, mgmt_tty, debug_tty, arm = parse(sys.argv)

    if arm:
        arm_debug(mgmt_tty)

    print("watching %s for %d s - provision on %s now" % (debug_tty, seconds, mgmt_tty))
    try:
        total, stats_lines, events = capture(seconds, debug_tty)
    except Exception as exc:
        print("capture failed: %s" % exc, file=sys.stderr)
        return 1

    print()
    print("captured %d lines, %d of them `stats:`" % (total, stats_lines))
    if events:
        print("association events:")
        for line in events[:12]:
            print("    " + line)
    if total == 0:
        print("nothing arrived - is debug enabled on %s?" % mgmt_tty)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
