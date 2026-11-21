#!/usr/bin/env python3
"""Run the MPK mini 3 emulator: the keyboard's firmware on an emulated AT32F415 with a front panel window.

usage: python3 mpk3_emu.py [custom|stock|IMAGE] [--reset] [--headless] [--fast]
       python3 mpk3_emu.py --stub-image OUT      (custom firmware with our own boot stub: shareable, see below)
  custom   fw/mpk.bin (built with make -C fw), the default
  stock    Akai's firmware from firmware/stock_v1.26/AT.bin (fetch it with firmware/get_stock_firmware.sh)
  IMAGE    any full 128 KB image (e.g. your own copy of Akai's AT.bin)
  --reset  start from a freshly flashed image (otherwise settings and stored data persist between runs)
While it runs, the repo's tools (mpk3_cfg.py, mpk3_gui.py, mpk3_market.py, mpk3.py, mpk3_tui.py) find it
before a real keyboard. Emulator details: emu/README.md.

A bundled build (the AppImage, emu/build_appimage.sh) sets MPK3_EMU_BIN to its emulator binary and
MPK3_EMU_CUSTOM to a prebuilt stub image, so nothing is compiled and no Akai code is needed for `custom`.
"""
import argparse
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
STATE = os.path.join(os.environ.get("XDG_DATA_HOME", os.path.expanduser("~/.local/share")), "mpk3-emu")
BUNDLED = os.environ.get("MPK3_EMU_BIN")           # set by a bundled build: use its files, compile nothing
IMAGES = {"custom": os.environ.get("MPK3_EMU_CUSTOM") or os.path.join(HERE, "fw", "mpk.bin"),
          "stock": os.path.join(HERE, "firmware", "stock_v1.26", "AT.bin")}
SETTINGS = 0x1E000  # custom firmware settings sector (fw/settings.h); everything below it is code
APP, IMG_LEN = 0xA000, 0x20000


def stub_image(app):
    """A full image of the custom firmware with our own 80-byte boot stub where Akai's bootloader sits. It does
    what the bootloader does before starting an app - interrupts off, jump to the reset vector at 0x0800A004 - so
    the image holds no Akai code and can be shared. (No update mode: that is the real bootloader's job.)"""
    img = bytearray(b"\xff" * IMG_LEN)
    struct.pack_into("<II", img, 0, 0x20002000, 0x08000041)       # initial SP, reset -> 0x40 (Thumb)
    for vec in range(2, 16):
        struct.pack_into("<I", img, 4 * vec, 0x0800004D)          # any other exception: stop at 0x4C
    struct.pack_into("<HHHHIH", img, 0x40,
                     0xB672,                                      # 0x40 cpsid i
                     0x4801,                                      # 0x42 ldr r0, [pc, #4]   (the word at 0x48)
                     0x6800,                                      # 0x44 ldr r0, [r0]       (app's reset vector)
                     0x4700,                                      # 0x46 bx r0
                     0x0800A004,                                  # 0x48
                     0xE7FE)                                      # 0x4C b .
    if len(app) > SETTINGS - APP:
        raise ValueError(f"app is {len(app)} bytes, more than fits below the settings sector")
    img[APP:APP + len(app)] = app
    return bytes(img)


def needs_flash(flash, image):
    """The emulated flash holds a different build of the firmware than `image` (custom only: stock keeps data)."""
    with open(flash, "rb") as f, open(image, "rb") as g:
        return f.read(SETTINGS) != g.read(SETTINGS)


def main():
    ap = argparse.ArgumentParser(description="MPK mini 3 emulator")
    ap.add_argument("image", nargs="?", default="custom", help="custom, stock or a 128 KB image file")
    ap.add_argument("--reset", action="store_true", help="flash the image again (forget settings and stored data)")
    ap.add_argument("--headless", action="store_true", help="no window (the panel socket is still there)")
    ap.add_argument("--fast", action="store_true", help="don't pace to real time")
    ap.add_argument("--stub-image", metavar="OUT", help="write the custom firmware (fw/mpk.app) with our boot stub")
    a = ap.parse_args()

    if a.stub_image:
        subprocess.run(["make", "-s", "-C", os.path.join(HERE, "fw"), "mpk.app"], check=True)
        with open(os.path.join(HERE, "fw", "mpk.app"), "rb") as f, open(a.stub_image, "wb") as out:
            out.write(stub_image(f.read()))
        print(f"{a.stub_image}: custom firmware with the boot stub (no Akai code)")
        return
    if not BUNDLED:
        subprocess.run(["make", "-s", "-C", os.path.join(HERE, "emu")], check=True)
        if a.image == "custom":
            subprocess.run(["make", "-s", "-C", os.path.join(HERE, "fw"), "mpk.bin"], check=True)
    image = IMAGES.get(a.image, a.image)
    if not os.path.isfile(image):
        sys.exit(f"{image} not found" + (" - fetch it with firmware/get_stock_firmware.sh" if a.image == "stock" else ""))

    os.makedirs(STATE, exist_ok=True)
    name = a.image if a.image in IMAGES else os.path.splitext(os.path.basename(image))[0]
    flash = os.path.join(STATE, name + ".flash")
    cmd = [BUNDLED or os.path.join(HERE, "emu", "mpk3emu"), flash]
    if a.reset or not os.path.exists(flash) or (a.image == "custom" and needs_flash(flash, image)):
        print(f"flashing {image} (stored settings are wiped, as on the keyboard)")
        cmd += ["--image", image]
    if a.fast:
        cmd.append("--fast")
    emu = subprocess.Popen(cmd)
    try:
        if a.headless:
            emu.wait()
        else:
            panel = subprocess.Popen([sys.executable, os.path.join(HERE, "emu", "panel.py")])
            while emu.poll() is None and panel.poll() is None:
                try:
                    emu.wait(0.2)
                except subprocess.TimeoutExpired:
                    pass
            panel.terminate()
    except KeyboardInterrupt:
        pass
    finally:
        if emu.poll() is None:
            emu.terminate()
            emu.wait()


if __name__ == "__main__":
    main()
