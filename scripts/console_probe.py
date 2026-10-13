#!/usr/bin/env python3
"""Probe a CDC-ACM console on the bridge: send one command, read its whole reply.

Usage:
    scripts/console_probe.py [device] [command ...]

Default device is /dev/ttyACM0, the management console.  Baud is irrelevant:
CDC-ACM is USB.

Why this reads by prompt instead of by timer: the console ends every reply with
a prompt, so a reply is finished exactly when "(set|scan|list|use|save) # "
arrives.  Sleeping a guessed number of seconds cuts a state dump in half, and
that is what looked like a truncated console.  Two rules keep the read honest:

  * Nothing already received is discarded.  An earlier version called
    reset_input_buffer() after writing the command, which threw away the echo
    and the first lines of the state dump - a complete dump then looked
    truncated on the host while the board had printed all of it.
  * One Enter per command.  CR-LF is two Enter characters, so the board ran the
    line twice; the second run is an empty line, which reprints the whole state
    dump.  This sends one CR.

One opener per port: a second process reading the same ttyACM splits the board's
output between them, which is what makes a console look mute.
"""
import sys
import time

try:
    import serial
except ImportError:
    print("pyserial is missing: sudo apt install python3-serial", file=sys.stderr)
    sys.exit(2)

PROMPT = b"# "                      # both prompts end with this
DEFAULT_PORT = "/dev/ttyACM0"


def read_until_prompt(port, deadline_s=10.0, quiet_s=1.5):
    """Read until the prompt arrives, the board goes quiet, or time runs out.

    The port is a parameter, not a global.  When read_until_prompt() read a
    module-level `ser` while open_synced() held the same port in a local, the
    first call after open_synced() raised NameError: name 'ser' is not defined -
    which looked, on the host, exactly like a dead console.

    A read can also raise SerialException ("device reports readiness to read but
    returned no data"), measured here on the first open made right after another
    session closed the same port.  That is the port going away under this reader,
    not the board dying, so it is returned as a failed read the caller can retry
    instead of an uncaught traceback.
    """
    buf = bytearray()
    started = time.monotonic()
    last_byte = started

    while True:
        try:
            chunk = port.read(256)
        except serial.SerialException as exc:
            return bytes(buf), "port closed (%s)" % exc
        now = time.monotonic()
        if chunk:
            buf += chunk
            last_byte = now
            if buf.endswith(PROMPT):
                return bytes(buf), "prompt"
        if (now - started) > deadline_s:
            return bytes(buf), "timeout"
        if buf and (now - last_byte) > quiet_s:
            return bytes(buf), "quiet"
        if (not buf) and ((now - started) > 4.0):
            return bytes(buf), "silent"


def open_synced(device, open_tries=3):
    """Open the port and get the board to answer with a prompt.

    Why the retry is not paranoia: measured on this desk, the first open made
    right after another session closed the same port returned no bytes at all -
    the probe printed its own header and nothing from the board, which reads on
    the host as a dead console. The same command, opened 3 s later, returned the
    full 510-byte state dump every time. Closing a CDC-ACM port is a mount/umount
    event for the device, and the firmware answers the new session only after it
    has finished servicing the old one, so the wait belongs between whole opens -
    not between the prompt probes inside one open, which is what the first version
    retried and therefore never fixed.

    Returns (serial, sync_bytes) or (None, reason).
    """
    for attempt in range(1, open_tries + 1):
        try:
            ser = serial.Serial(port=device, baudrate=115200, timeout=0.2)
        except Exception as exc:
            print("[%s] open failed (try %d): %s" % (device, attempt, exc),
                  file=sys.stderr)
            time.sleep(2.0)
            continue

        # The only discard this script does, and it happens before anything is
        # written: the banner and any periodic line already in flight are not
        # part of the reply.
        ser.reset_input_buffer()

        # Sync on the prompt: an empty line makes the board reprint its state and
        # the prompt, so the reply asked for below has a known end. A board that is
        # past its banner (no reboot since the last session) stays silent until
        # asked, so ask a few times before calling this open unusable.
        for attempt in range(3):
            ser.write(b"\r")
            ser.flush()
            sync, how = read_until_prompt(ser, deadline_s=6.0)
            if sync.endswith(PROMPT):
                return ser, sync
            if how.startswith("port closed"):
                break                       # the port went away; reopen it
            if sync:
                sys.stderr.write(sync.decode("utf-8", "replace"))

        try:
            ser.close()
        except Exception:
            pass
        print("[%s] no prompt from this open (%s); reopening" % (device, how),
              file=sys.stderr)
        time.sleep(2.0)

    return None, "no prompt after %d opens" % open_tries


device = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PORT
cmd_text = " ".join(sys.argv[2:])

for attempt in range(2):
    ser, sync = open_synced(device)
    if ser is None:
        print("[!] %s unusable: %s" % (device, sync), file=sys.stderr)
        sys.exit(3)

    print("--- %s ---" % device)
    print(">> %s" % (cmd_text if cmd_text else "(empty line)"))
    try:
        ser.write((cmd_text + "\r").encode())
        ser.flush()
    except serial.SerialException as exc:
        print("[!] port closed before the command was written: %s" % exc,
              file=sys.stderr)
        time.sleep(2.0)
        continue

    reply, how = read_until_prompt(ser)
    sys.stdout.write(reply.decode("utf-8", "replace"))
    sys.stdout.flush()
    print("<< %d bytes, ended on %s" % (len(reply), how))

    try:
        ser.close()
    except Exception:
        pass

    if how.startswith("port closed") and attempt + 1 < 2:
        time.sleep(2.0)                     # a fresh open can finish the read
        continue
    break

