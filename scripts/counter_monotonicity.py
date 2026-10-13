#!/usr/bin/env python3
"""Monotonicity check for the bridge counters.

Why this exists: a frame counter cannot decrease.  Measured on this desk,
`->wifi`, `->host` and `rxdrop` moved *backwards* between two reads of `stats`.
That is either the firmware resetting the counters or the reader being wrong,
and the two are told apart here: the same numbers are read from two independent
paths inside one window - the `stats` command on ttyACM0 and the periodic line
on ttyACM1 - and every sample comes from a session that stays open across the
whole run, so no open/close boundary sits between two samples.

If the numbers still move backwards with one opener held open the whole time,
the firmware is the thing that resets them.  If they only moved backwards
between separate opens, the reader was the problem.

Two things the run has to get right before its numbers mean anything:
  * `debug: off` is the state after every boot, so ttyACM1 is silent until
    `set debug on` is sent on ttyACM0.  Without that arming the periodic path
    returns nothing and reads as "(none)" - which is the same output a board
    that never prints would produce.  This script arms it once, at the start.
  * The gap has to be at least BRIDGE_STATUS_INTERVAL_MS (15 s), or the
    periodic path samples a console that simply has not spoken yet.

Usage: scripts/counter_monotonicity.py [rounds] [gap-seconds]
"""
import sys
import time

try:
    import serial
except ImportError:
    print("pyserial is missing: sudo apt install python3-serial", file=sys.stderr)
    sys.exit(2)

ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 6
# 16 s, not 4: the periodic line only arrives every BRIDGE_STATUS_INTERVAL_MS
# (15 s), so a shorter gap samples a console that has not spoken yet and reports
# it as "(none)".
GAP_S = float(sys.argv[2]) if len(sys.argv) > 2 else 16.0


def stats_lines(text):
    return [line for line in text.splitlines() if line.startswith("stats:")]


mgmt = serial.Serial(port="/dev/ttyACM0", baudrate=115200, timeout=0.2)
debug = serial.Serial(port="/dev/ttyACM1", baudrate=115200, timeout=0.2)
mgmt.reset_input_buffer()
debug.reset_input_buffer()

# Arm the debug console once, before any sample is taken.  `debug: off` is the
# state after every boot, so without this the ttyACM1 path is silent and every
# round prints "(none)" for it - indistinguishable from a board that never
# prints.  The reply is drained so it cannot leak into the first sample.
mgmt.write(b"set debug on\r")
mgmt.flush()
arm = bytearray()
arm_started = time.monotonic()
while True:
    chunk = mgmt.read(256)
    if chunk:
        arm += chunk
        if arm.endswith(b"# "):
            break
    if (time.monotonic() - arm_started) > 8.0:
        break
print("armed: %d bytes read while enabling debug" % len(arm))

time.sleep(1.0)

for round_no in range(ROUNDS):
    mgmt.write(b"stats\r")
    mgmt.flush()

    buf = bytearray()
    started = time.monotonic()
    while (not buf.endswith(b"# ")) and ((time.monotonic() - started) < 10.0):
        chunk = mgmt.read(256)
        if chunk:
            buf += chunk

    # Drain the periodic stream for the whole gap.  The periodic line only
    # arrives every BRIDGE_STATUS_INTERVAL_MS (15 s), so a short drain would
    # sample a console that simply has not spoken yet and report it as empty.
    streamed = bytearray()
    drain_until = time.monotonic() + GAP_S
    while time.monotonic() < drain_until:
        chunk = debug.read(512)
        if chunk:
            streamed += chunk

    probed = stats_lines(buf.decode("utf-8", "replace"))
    periodic = stats_lines(streamed.decode("utf-8", "replace"))
    print("round %d  probe    %s" % (round_no, probed[0] if probed else "(none)"))
    for line in periodic:
        print("round %d  periodic %s" % (round_no, line))
    time.sleep(GAP_S)

mgmt.close()
debug.close()
