#!/usr/bin/env python3
"""Walk the board through its states and report the LED pattern chosen in each.

`led_probe.py` times a waveform.  `led_choice_probe.py` checks the one choice that
needs no credentials (`double`, during a live scan).  This covers the part of
table 3 that only a working uplink makes reachable:

    solid   associated                     needs --ssid / --pass
    double  a live scan                    needs no credentials
    1hz     a profile that cannot join     needs --dead
    5hz     no profile configured          reported, not driven: reaching it means
                                           deleting the saved profile
    off     USB not ready                  cannot be read from a host that is
                                            reading the console at all

The onboard LED is a CYW43 GPIO and cannot be read back: cyw43_ll_gpio_get()
sends the "ccgpioin" ioctl, the radio's input state, not the "gpioout" value the
firmware wrote.  Every reading below is the chooser's own decision, as the board
reports it through `test led`.

Credentials are arguments, never literals - nothing in this file is a password.

Needs an image built with -DBRIDGE_SELFTEST=1 (build-selftest).

Usage:
    scripts/led_state_matrix.py [tty] --ssid SSID --pass PASSWORD [--dead SSID]

    scripts/led_state_matrix.py /dev/ttyACM0 --ssid MyNetwork --pass hunter2
"""
import re
import sys
import time

import serial

LED_RE = re.compile(r"led:\s+(\S+)\s+level\s+(\S+)\s+tick\s+(\d+)\s+state asks\s+(\S+)")

# A join attempt has a 20 s deadline per attempt.  Reading sooner than that reports
# the state the command replaced, not the one it caused.
SETTLE_SECONDS = 30

# A network name that is not going to resolve to an access point: this is how the
# "configured but not associated" state is produced on purpose.
DEAD_SSID = "pico2-selftest-absent"

# What each pattern means in table 3, so a reading can be judged against it.
MEANING = {
    "solid": "associated",
    "double": "live scan",
    "1hz": "configured, not associated",
    "5hz": "no Wi-Fi configured",
    "off": "USB not ready",
}


def parse(argv):
    """Positional tty, then --ssid / --pass / --dead.  No credential lives here."""
    port = "/dev/ttyACM0"
    ssid = None
    password = None
    dead = "pico2-selftest-absent"

    args = sys.argv[1:]
    index = 0
    while index < len(args):
        item = args[index]
        if item in ("--ssid", "--pass", "--dead") and index + 1 < len(args):
            value = args[index + 1]
            if item == "--ssid":
                ssid = value
            elif item == "--pass":
                password = value
            else:
                dead = value
            index += 2
        elif item.startswith("/dev/"):
            port = item
            index += 1
        else:
            index += 1
    return port, ssid, password, dead


def drain(stream, seconds):
    """Collect everything the console says for `seconds`."""
    buf = bytearray()
    deadline = time.time() + seconds
    while time.time() < deadline:
        chunk = stream.read(4096)
        if chunk:
            buf += chunk
        else:
            time.sleep(0.05)
    return buf.decode("utf-8", "replace")


class Console:
    """The management console, reopened whenever the port has gone.

    A live scan can take the CDC-ACM function down mid-session, and this walk runs
    for a couple of minutes, so one reopen is built in rather than assumed away.
    """

    def __init__(self, port):
        self.port = port
        self.stream = None

    def open(self):
        if self.stream is None:
            self.stream = serial.Serial(self.port, 115200, timeout=0.5)
            time.sleep(0.4)
            self.stream.reset_input_buffer()

    def command(self, line, seconds=2.5):
        """Send one line, return what came back, reopening the port once on failure."""
        for attempt in (0, 1):
            try:
                if self.stream is None:
                    self.open()
                self.stream.write((line + "\r\n").encode())
                self.stream.flush()
                return drain(self.stream, seconds)
            except Exception as exc:
                print("    (port dropped: %s - reopening)" % exc)
                self.close()
                time.sleep(1.0)
        return ""

    def close(self):
        if self.stream is not None:
            try:
                self.stream.close()
            except Exception:
                pass
            self.stream = None


def choice(console):
    """Ask the board what it is driving: (pattern, level, tick, pattern-the-state-asks)."""
    text = console.command("test led")
    match = LED_RE.search(text)
    if match is None:
        return None
    return match.group(1), match.group(2), int(match.group(3)), match.group(4)


def check(console, expected, why):
    """Report whether the state machine chose `expected` for the state it is in."""
    read = choice(console)
    if read is None:
        print("    %-6s (%-28s) no `test led` readout" % (expected, why))
        return False

    driven, level, tick, asked = read
    ok = (driven == expected) and (asked == expected)
    print("    %-6s (%-28s) driven=%-6s level=%-3s asks=%-6s %s"
          % (expected, why, driven, level, asked, "match" if ok else "MISMATCH"))
    return ok


def main(argv):
    port, ssid, password, dead = parse(argv)
    if ssid is None or password is None:
        print(__doc__, file=sys.stderr)
        return 2

    reached = set()
    console = Console(port)

    try:
        print("state the board is in now:")
        for line in console.command("stats", 3.0).splitlines():
            if line.strip():
                print("    " + line.strip())

        if check(console, "solid", "associated"):
            reached.add("solid")

        console.command("scan")
        console.command("live")
        if check(console, "double", "live scan running"):
            reached.add("double")
        console.command("back")

        console.command("set ssid " + dead)
        console.command("set pass deliberately-wrong")
        print("waiting %d s for that join to fail ..." % SETTLE_SECONDS)
        time.sleep(SETTLE_SECONDS)
        if check(console, "1hz", "profile saved, no association"):
            reached.add("1hz")

        console.command("set ssid " + ssid)
        console.command("set pass " + password)
        print("re-joining the real network ...")
        time.sleep(SETTLE_SECONDS)
        if check(console, "solid", "associated again"):
            reached.add("solid")
    finally:
        console.close()

    print()
    print("choices confirmed by this run: %s"
          % (", ".join(sorted(reached)) if reached else "none"))
    for pattern in ("5hz", "off"):
        print("%-6s not driven here: %s" % (pattern, MEANING[pattern]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

