#!/usr/bin/env python3
"""bin2uf2.py — convert firmware.bin to a UF2 for the ClearCore boot drive.

UF2 block format per https://github.com/microsoft/uf2 (MIT).
Defaults: base address 0x4000 (app start after the Teknic bootloader),
family 0x55114460 (SAMD51/SAME5x). If the boot drive silently rejects the
file, check INFO_UF2.TXT on the drive and retry with --no-family.

Usage: bin2uf2.py [-o out.uf2] [--base 0x4000] [--no-family] firmware.bin
MIT License, Copyright (c) 2026 Craig Hollabaugh
"""

import argparse
import struct
import sys

MAGIC0, MAGIC1, MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
FLAG_FAMILY = 0x2000
FAMILY_SAME5X = 0x55114460
PAYLOAD = 256

def convert(data, base, family):
    nblocks = (len(data) + PAYLOAD - 1) // PAYLOAD
    out = bytearray()
    for i in range(nblocks):
        chunk = data[i * PAYLOAD:(i + 1) * PAYLOAD].ljust(PAYLOAD, b"\x00")
        flags = FLAG_FAMILY if family else 0
        hd = struct.pack("<IIIIIIII", MAGIC0, MAGIC1, flags,
                         base + i * PAYLOAD, PAYLOAD, i, nblocks,
                         family if family else 0)
        out += hd + chunk + b"\x00" * (512 - len(hd) - PAYLOAD - 4)
        out += struct.pack("<I", MAGIC_END)
    return bytes(out)

if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("binfile")
    p.add_argument("-o", "--out")
    p.add_argument("--base", type=lambda x: int(x, 0), default=0x4000)
    p.add_argument("--no-family", action="store_true")
    a = p.parse_args()

    with open(a.binfile, "rb") as f:
        data = f.read()

    out = a.out or (a.binfile.rsplit(".", 1)[0] + ".uf2")
    with open(out, "wb") as f:
        f.write(convert(data, a.base, 0 if a.no_family else FAMILY_SAME5X))

    print("%s: %d bytes -> %s (%d blocks @ 0x%x)"
          % (a.binfile, len(data), out, (len(data) + PAYLOAD - 1) // PAYLOAD, a.base))
