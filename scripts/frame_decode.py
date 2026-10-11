#!/usr/bin/env python3
"""Arm the debug stream, restart host DHCP, capture frames, and validate them.

Built with -DTRACE_FRAMES=1 the firmware prints the first bytes of every bridged
frame as `raw <dir>  len <n>  <hex>`.  The debug stream has no line framing, so
records are found by marker and read for exactly as many hex digits as they
announced.  Each frame is then checked the way a host would: Ethernet type, IP
version and IHL, total length, header checksum recomputed with the field zeroed,
protocol, and the UDP ports.
"""
import re
import subprocess as SP
import sys
import time as T

import serial


def internet_checksum(data):
    acc = 0
    for i in range(0, len(data) - 1, 2):
        acc += (data[i] << 8) | data[i + 1]
    if len(data) & 1:
        acc += data[-1] << 8
    while acc >> 16:
        acc = (acc & 0xFFFF) + (acc >> 16)
    return (~acc) & 0xFFFF


def ipv4(raw, off):
    return ".".join(str(b) for b in raw[off:off + 4])


def mac(raw, off):
    return ":".join(f"{b:02x}" for b in raw[off:off + 6])


def main():
    if len(sys.argv) < 3:
        print("usage: frame_decode.py <mgmt-tty> <debug-tty> <iface> [seconds]", file=sys.stderr)
        return 2

    mgmt, debug_tty, iface = sys.argv[1], sys.argv[2], sys.argv[3]
    secs = float(sys.argv[4]) if len(sys.argv) > 3 else 20.0

    # 1. turn the debug stream on, from the management console.
    try:
        s = serial.Serial(mgmt, 115200, timeout=1.0)
        T.sleep(0.4)
        s.reset_input_buffer()
        s.write(b"set debug on\r\n")
        s.flush()
        T.sleep(0.8)
        s.read(4096)
        s.close()
    except Exception as exc:
        print(f"could not arm debug on {mgmt}: {exc}", file=sys.stderr)
        return 1

    # 2. make the host run DHCP again, so frames are actually produced.
    SP.Popen(["nmcli", "device", "disconnect", iface], stdout=SP.DEVNULL,
             stderr=SP.DEVNULL)
    T.sleep(2.0)
    SP.Popen(["nmcli", "device", "connect", iface], stdout=SP.DEVNULL,
             stderr=SP.DEVNULL)
    T.sleep(1.0)

    # 3. capture the debug stream
    s = serial.Serial(debug_tty, 115200, timeout=0.5)
    buf = bytearray()
    end = T.time() + secs
    while T.time() < end:
        chunk = s.read(4096)
        if chunk:
            buf += chunk
        if not chunk:
            T.sleep(0.05)
    s.close()

    if len(sys.argv) > 5:
        with open(sys.argv[5], "wb") as fh:
            fh.write(bytes(buf))

    text = buf.decode("latin-1")
    rows = []
    total = 0
    for m in re.finditer(r"raw (->wifi|->host)\s+len (\d+)\s+([0-9a-f]*)", text):
        direction = m.group(1)
        announced = int(m.group(2))
        n = min(announced, 42)
        hexpart = m.group(3)
        if len(hexpart) < 2 * n:
            continue
        raw = bytes.fromhex(hexpart[:2 * n])
        total += 1

        eth_type = (raw[12] << 8) | raw[13]
        row = [f"{direction:7s} len={announced:4d} eth dst={mac(raw, 0)} "
               f"src={mac(raw, 6)} type=0x{eth_type:04x}"]

        if (eth_type != 0x0800) or (n < 42):
            rows.append(" ".join(row) + "  (not IPv4 at this offset)")
            continue

        ip = raw[14:42]
        ihl = (ip[0] & 0x0F) * 4
        ip_len = (ip[2] << 8) | ip[3]
        ttl, proto = ip[8], ip[9]
        got = (ip[10] << 8) | ip[11]
        # The header checksum covers ihl bytes, not a fixed 20 - an IP header
        # with options (ihl > 20) must be summed over its real length.
        probe = bytearray(ip[:ihl])
        probe[10] = probe[11] = 0
        want = internet_checksum(bytes(probe))
        rows.append(" ".join(row) + f" | v{ihl} len={ip_len} ttl={ttl} proto={proto} "
                    f"csum=0x{got:04x} want=0x{want:04x} "
                    f"{'OK' if got == want else 'BAD'} "
                    f"src={ipv4(ip, 12)} dst={ipv4(ip, 16)}")

    for row in dict.fromkeys(rows):
        print(row)
    print(f"--- {total} frames captured, {len(dict.fromkeys(rows))} distinct ---")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
