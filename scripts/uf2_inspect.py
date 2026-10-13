#!/usr/bin/env python3
"""Decode the headers of a UF2 image and report what its load aims at.

Every constant here is copied from the authoritative layout - struct uf2_block
in .pico-sdk/src/common/boot_uf2_headers/include/boot/uf2.h - not read off a hex
dump.

Why this exists: the settings record has to survive a reflash, and a UF2 built
for an RP2350 carries the RP2350-E10 absolute block (family ABSOLUTE, extension
UF2_EXTENSION_RP2_IGNORE_BLOCK) at the top of the flash window.  On a 4 MB part
that address lands in the final 4 kB sector, and the erase which accompanies a
BOOTSEL drag-and-drop covers the sector it lands in whatever the size of the
image - pico-sdk#1943: "this drag & drop will unnecessarily erase the last 4KiB
of connected flash", while picotool "will ignore any E10 abs-blocks".

This answers, from the image alone and without a board, whether any block of an
image aims at the sector the settings record lives in.

  python3 scripts/uf2_inspect.py                       # every built image
  python3 scripts/uf2_inspect.py build/pico2_wifi_card.uf2
  python3 scripts/uf2_inspect.py --reserve 16 <uf2>    # guard region changed
  python3 scripts/uf2_inspect.py --all <uf2>           # dump every header

Exit code 0 = every header decoded and no block aims at the settings sector.
"""
import glob
import os
import struct
import sys

# --- constants, copied from boot/uf2.h ---------------------------------------
MAGIC_START0 = 0x0A324655
MAGIC_START1 = 0x9E5D5157
MAGIC_END = 0x0AB16F30

FLAG_NAMES = [
    (0x00000001, "NOT_MAIN_FLASH"),
    (0x00001000, "FILE_CONTAINER"),
    (0x00002000, "FAMILY_ID_PRESENT"),
    (0x00004000, "MD5_PRESENT"),
    (0x00008000, "EXTENSION_FLAGS"),
]
FLAG_EXTENSION = 0x00008000
FLAG_KNOWN = 0x00000001 | 0x00001000 | 0x00002000 | 0x00004000 | FLAG_EXTENSION

FAMILIES = {
    0xE48BFF55: "CYW43_FIRMWARE",
    0xE48BFF56: "RP2040",
    0xE48BFF57: "ABSOLUTE",
    0xE48BFF58: "DATA",
    0xE48BFF59: "RP2350_ARM_S",
    0xE48BFF5A: "RP2350_RISCV",
    0xE48BFF5B: "RP2350_ARM_NS",
}
FAMILY_ABSOLUTE = 0xE48BFF57

EXTENSION_IGNORE_BLOCK = 0x9957E304
EXTENSION_NAMES = [(0x9957E304, "RP2_IGNORE_BLOCK")]

BLOCK_SIZE = 512
EXTENSION_AT = 32 + 256      # header(32) + UF2_PAGE_SIZE(256): elf2uf2.cpp writes
                             # the extension flags at data[UF2_PAGE_SIZE]
XIP_BASE = 0x10000000
FLASH_SIZE = 4 * 1024 * 1024     # pico2_w: PICO_FLASH_SIZE_BYTES = 4 MB
SECTOR = 4096


def flags_text(flags):
    parts = [name for bit, name in FLAG_NAMES if flags & bit]
    rest = flags & ~FLAG_KNOWN
    if rest:
        parts.append("0x%08x" % rest)
    return "|".join(parts) if parts else "0"


def extension_text(row):
    """The extension flags of a block, or '-' when the flag bit is not set."""
    if not (row["flags"] & FLAG_EXTENSION):
        return "-"
    value, = struct.unpack("<I", row["blk"][EXTENSION_AT:EXTENSION_AT + 4])
    names = [name for bit, name in EXTENSION_NAMES if value & bit]
    return "|".join(names) if names else "0x%08x" % value



def describe(row):
    return ("blk %4d/%-4d  target 0x%08x  payload %4d  flags 0x%04x(%s)  family %s  ext %s"
            % (row["block_no"] + 1, row["num_blocks"], row["target"], row["payload_len"],
               row["flags"], flags_text(row["flags"]),
               FAMILIES.get(row["family"], "0x%08x" % row["family"]),
               extension_text(row)))


def decode(path, flash_size, reserve, show_all):
    """Decode every block header of a UF2 and report what it aims at."""
    with open(path, "rb") as fh:
        raw = fh.read()

    print("=== %s  (%d bytes)" % (path, len(raw)))
    if (len(raw) == 0) or (len(raw) % BLOCK_SIZE):
        print("    FAIL: size is not a non-zero multiple of 512 - not a UF2 of boot blocks")
        return 1

    settings_offs = flash_size - reserve * SECTOR
    settings_lo = XIP_BASE + settings_offs
    settings_hi = settings_lo + SECTOR - 1
    print("    settings sector  flash 0x%06x..0x%06x  XIP 0x%08x..0x%08x  (%d sectors reserved)"
          % (settings_offs, settings_offs + SECTOR - 1, settings_lo, settings_hi, reserve))

    rows = []
    bad = 0
    for i in range(len(raw) // BLOCK_SIZE):
        blk = raw[i * BLOCK_SIZE:(i + 1) * BLOCK_SIZE]
        (magic0, magic1, flags, target, payload_len,
         block_no, num_blocks, family) = struct.unpack("<8I", blk[0:32])
        magic_end, = struct.unpack("<I", blk[508:512])
        if ((magic0 != MAGIC_START0) or (magic1 != MAGIC_START1) or
                (magic_end != MAGIC_END) or (payload_len > 256)):
            bad += 1
        rows.append(dict(i=i, flags=flags, target=target, payload_len=payload_len,
                         block_no=block_no, num_blocks=num_blocks, family=family,
                         offset=target - XIP_BASE, blk=blk))

    lo_target = min(row["target"] for row in rows)
    hi_target = max(row["target"] + max(row["payload_len"], 1) - 1 for row in rows)
    print("    blocks %d   bad headers %d" % (len(rows), bad))
    print("    target range  0x%08x .. 0x%08x   flash offset 0x%06x .. 0x%06x"
          % (lo_target, hi_target, lo_target - XIP_BASE, hi_target - XIP_BASE))
    print("    families  %s"
          % ", ".join(sorted({FAMILIES.get(row["family"], "0x%08x" % row["family"])
                              for row in rows})))
    print("    flags     %s"
          % ", ".join(sorted({"0x%08x(%s)" % (row["flags"], flags_text(row["flags"]))
                              for row in rows})))

    if show_all:
        for row in rows:
            print("      %s" % describe(row))

    abs_rows = [row for row in rows if row["family"] == FAMILY_ABSOLUTE]
    print("    ABSOLUTE blocks %d   picotool ignores them; a BOOTSEL drag-and-drop does not"
          % len(abs_rows))
    for row in abs_rows:
        print("      %s" % describe(row))

    hits = []
    for row in rows:
        span = max(row["payload_len"], 1)
        # A block hits the record either directly, or after the flash folds its
        # offset into the chip - which is exactly what the E10 absolute block does:
        # 0x10ffff00 on a 4 MB part is flash offset 0x3fff00, the last sector.
        for base in (row["target"], XIP_BASE + (row["offset"] % flash_size)):
            if (base <= settings_hi) and (base + max(row["payload_len"], 1) - 1 >= settings_lo):
                hits.append(row)
                break

    if bad:
        print("    FAIL: %d block(s) with bad magic or oversized payload" % bad)
    if hits:
        print("    blocks aiming at the settings sector: %d" % len(hits))
        for row in hits:
            print("      HIT  %s" % describe(row))
        print("    FAIL: a load of this image touches the sector the record lives in")
    else:
        print("    blocks aiming at the settings sector: 0   OK")
    return 1 if (bad or hits) else 0


def main(argv):
    reserve = 16
    flash_size = FLASH_SIZE
    show_all = False
    paths = []

    argv = argv[1:]
    i = 0
    while argv:
        arg = argv.pop(0)
        if arg == "--reserve":
            reserve = int(argv.pop(0), 0)
        elif arg == "--flash-size":
            flash_size = int(argv.pop(0), 0)
        elif arg == "--all":
            show_all = True
        elif arg in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            paths.append(arg)

    if not paths:
        paths = sorted(glob.glob("build*/pico2_wifi_card.uf2"))
        if not paths:
            print("no uf2 given and none found in build*/", file=sys.stderr)
            return 1

    rc = 0
    for path in paths:
        rc |= decode(path, flash_size, reserve, show_all)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))


