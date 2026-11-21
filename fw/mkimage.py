"""Wrap app.bin (linked at 0x0800A000) into a full 128 KB image the stock bootloader accepts.

Layout: stock 0x0000-0x9FFF (bootloader + copy) | app | 0xFF | trailer (VID/PID from stock, version, checksum).
usage: python3 fw/mkimage.py app.bin out.bin [version, default 0001]
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from mpk3_flash import BOOT_END, IMG_LEN, STOCK, check_image, checksum  # noqa: E402

TRAILER = IMG_LEN - 16


def build(app, stock, version=0x0001):
    if len(app) > TRAILER - BOOT_END:
        raise ValueError(f"app is {len(app)} bytes, max {TRAILER - BOOT_END}")
    sp, reset = struct.unpack_from("<II", app)
    if not (0x20000000 < sp <= 0x20008000 and 0x0800A000 < reset < 0x08000000 + TRAILER and reset & 1):
        raise ValueError(f"bad vector table: sp={sp:#x} reset={reset:#x}")
    if version & 0xFF == 0xFF or version >> 8 == 0xFF:
        raise ValueError("version bytes must not be 0xFF (bootloader treats that as 'no app')")
    img = bytearray(stock[:BOOT_END] + app + b"\xff" * (TRAILER - BOOT_END - len(app)) + stock[TRAILER:])
    img[0x1FFF6:0x1FFF8] = version.to_bytes(2, "big")
    img[-2:] = checksum(img).to_bytes(2, "big")
    errs = check_image(bytes(img), stock)
    assert not errs, errs
    return bytes(img)


if __name__ == "__main__":
    app, out = sys.argv[1], sys.argv[2]
    ver = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x0001
    img = build(open(app, "rb").read(), open(STOCK, "rb").read(), ver)
    open(out, "wb").write(img)
    print(f"{out}: app {os.path.getsize(app)} bytes, version {ver:04x}, checksum {img[-2:].hex()}")
