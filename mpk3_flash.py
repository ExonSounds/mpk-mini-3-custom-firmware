"""Flash an MPK mini 3 (AT32 variant) through Akai's stock USB HID bootloader.

Protocol is from Akai's updater v1.26; see firmware/NOTES.md.
Put the keyboard in update mode first: hold BANK + PROG SELECT while plugging in USB.

usage: python3 mpk3_flash.py [image.bin]   (default: firmware/stock_v1.26/AT.bin)
"""
import glob
import os
import select
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
STOCK = os.path.join(HERE, "firmware", "stock_v1.26", "AT.bin")
IMG_LEN = 0x20000
BOOT_END = 0xA000  # bootloader + its copy; must stay byte-identical to stock
VID, PID = 0x09E8, 0x1049
CMD_ERASE, CMD_DATA, CMD_END, CMD_CONNECT = 0xC1, 0xC2, 0xC3, 0xC4  # C4 reply: UI F4 <ver hi> <ver lo>
TIMEOUT_S, RETRIES = 3.0, 3


def checksum(img):
    return sum(img[:IMG_LEN - 2]) & 0xFFFF


def check_image(img, stock):
    """Return a list of problems; empty means safe to flash."""
    if len(img) != IMG_LEN:
        return [f"image is {len(img)} bytes, expected {IMG_LEN}"]
    errs = []
    if img[0x1FFF0:0x1FFF4] != bytes([VID >> 8, VID & 0xFF, PID >> 8, PID & 0xFF]):
        errs.append("trailer VID/PID is not 09e8:1049 (wrong hardware variant?)")
    if int.from_bytes(img[-2:], "big") != checksum(img):
        errs.append(f"checksum mismatch: stored {img[-2:].hex()}, computed {checksum(img):04x}")
    if img[:BOOT_END] != stock[:BOOT_END]:
        errs.append("boot region 0x0000-0x9FFF differs from stock; refusing (could brick the bootloader)")
    return errs


def cmd(code):
    return bytes([0x55, 0x4F, code]) + bytes(29)


def data_packet(addr, seq, chunk):
    assert len(chunk) == 16
    return (bytes([0x55, 0x4F, CMD_DATA]) + addr.to_bytes(4, "big") + seq.to_bytes(2, "big")
            + chunk + bytes(5) + (sum(chunk) & 0xFFFF).to_bytes(2, "big"))


def find_hidraw():
    for dev in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        with open(os.path.join(dev, "device", "uevent")) as f:
            uevent = f.read()
        if f"{VID:08X}:" in uevent:
            ident = next(l for l in uevent.splitlines() if l.startswith("HID_ID="))
            return "/dev/" + os.path.basename(dev), ident
    return None, None


def xfer(fd, pkt):
    code = pkt[2] & 0x0F
    for attempt in range(RETRIES):
        os.write(fd, b"\x00" + pkt)  # report ID 0: the device uses unnumbered reports
        while select.select([fd], [], [], TIMEOUT_S)[0]:
            r = os.read(fd, 64)
            if r[:2] != b"UI":
                continue  # unrelated input report
            if r[2] == 0xF0 | code:  # F<n> = success, E<n> = failure (checked in the updater's branches)
                return r
            if pkt[2] == CMD_CONNECT:
                raise RuntimeError(f"keyboard did not accept connect (reply {r[:5].hex(' ')}). "
                                   "Unplug it, hold BANK + PROG SELECT, plug it back in.")
            raise RuntimeError(f"device rejected command {pkt[2]:#x}: reply {r[:4].hex(' ')}")
        print(f"  timeout on {pkt[2]:#x}, retry {attempt + 1}/{RETRIES}", file=sys.stderr)
    raise RuntimeError(f"no reply to command {pkt[2]:#x}")


def flash(fd, img, progress=print):
    # Same order as Akai's updater: Connect (C4), then erase, version, data, end.
    r = xfer(fd, cmd(CMD_CONNECT))  # raises unless the bootloader accepts, so erase is never sent otherwise
    progress(f"update mode, bootloader reports v{r[3]:x}.{r[4]:02x}; erasing...")
    xfer(fd, cmd(CMD_ERASE))
    xfer(fd, cmd(CMD_CONNECT))
    n = IMG_LEN // 16
    for seq in range(n):
        xfer(fd, data_packet(seq * 16, seq, img[seq * 16:seq * 16 + 16]))
        if seq % 512 == 511:
            progress(f"  {100 * (seq + 1) // n}%")
    xfer(fd, cmd(CMD_END))


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else STOCK
    img, stock = open(path, "rb").read(), open(STOCK, "rb").read()
    errs = check_image(img, stock)
    if errs:
        sys.exit("refusing to flash:\n  " + "\n  ".join(errs))
    node, ident = find_hidraw()
    if not node:
        sys.exit("no Akai HID device found. Hold BANK + PROG SELECT while plugging in USB.")
    print(f"{node} ({ident}), image {path} v{img[0x1FFF6]:x}.{img[0x1FFF7]:02x}")
    fd = os.open(node, os.O_RDWR)
    try:
        flash(fd, img)
    finally:
        os.close(fd)
    print("done. Unplug and replug the keyboard.")


if __name__ == "__main__":
    main()
