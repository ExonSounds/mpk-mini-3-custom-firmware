"""Use the keyboard as a remote screen and controller for programs running on this computer.

Starts the RAM program programs/remote.bin on the keyboard (build it with: make -C programs; needs firmware v9.80)
and talks to it over SysEx 0B: frames go to the screen, the controls come back with every frame.
    with Remote() as r:
        scr = Screen(); scr.text(0, 0, "HELLO"); controls = r.show(scr.fb)
"""
import os
import re
import time
from dataclasses import dataclass

from mpk3_cfg import IMG_H, IMG_W, PROG_MSG, Device, set_pixel

HERE = os.path.dirname(os.path.abspath(__file__))
REMOTE_BIN = os.path.join(HERE, "programs", "remote.bin")
FRAME, QUIT = 1, 2  # programs/remote.c ops


def pack7(data):
    """8 bytes per 7: a byte with the top bits, then the 7 low parts (unpack7 in programs/remote.c)."""
    out = bytearray()
    for i in range(0, len(data), 7):
        g = data[i:i + 7]
        out.append(sum((b >> 7 & 1) << j for j, b in enumerate(g)))
        out += bytes(b & 127 for b in g)
    return bytes(out)


@dataclass
class Controls:
    pads: tuple      # pads 1-8: 0 = up, 1-127 live pressure
    joy_x: int       # -1000 (left) .. 1000 (right)
    joy_y: int       # -1000 (down) .. 1000 (up)
    buttons: int     # bit = MPK_BTN_* (fw/prog_api.h)
    keys: tuple      # 25 velocities, 0 = up
    knobs: tuple     # 8 pointer positions 0-127


def idle_controls(**kw):
    """Nothing touched; override fields by name, e.g. idle_controls(pads=(0,) * 7 + (127,))."""
    c = dict(pads=(0,) * 8, joy_x=0, joy_y=0, buttons=0, keys=(0,) * 25, knobs=(64,) * 8)
    c.update(kw)
    return Controls(**c)


def pressed(before, now):
    """Indexes that went from up (0) to down since the last frame: pads or keys."""
    return [i for i, (a, b) in enumerate(zip(before, now)) if not a and b]


def parse_controls(r):
    if len(r) != 47:
        raise ConnectionError("the keyboard's remote screen stopped (PROG SELECT pressed, or replugged)")
    return Controls(tuple(r[:8]), (r[8] << 7 | r[9]) - 1000, (r[10] << 7 | r[11]) - 1000, r[12] << 7 | r[13],
                    tuple(r[14:39]), tuple(r[39:47]))


def load_font():
    """The firmware's 5x7 font (fw/gfx.c), ASCII 0x20-0x5F as 5 column bytes, bit 0 = top."""
    with open(os.path.join(HERE, "fw", "gfx.c")) as f:
        src = f.read()
    body = src[src.index("font5x7[64][5] = {"):]
    body = body[:body.index("};")]
    cols = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
    return [cols[i:i + 5] for i in range(0, 320, 5)]


class Screen:
    """A 128x32 1-bit frame in the keyboard's own layout, with the firmware's drawing calls."""
    font = None

    def __init__(self):
        self.fb = bytearray(512)
        if Screen.font is None:
            Screen.font = load_font()

    def clear(self):
        self.fb[:] = bytes(512)

    def px(self, x, y, on=True):
        set_pixel(self.fb, x, y, on)

    def rect(self, x, y, w, h, on=True):
        for i in range(max(x, 0), min(x + w, IMG_W)):
            for j in range(max(y, 0), min(y + h, IMG_H)):
                set_pixel(self.fb, i, j, on)

    def text(self, x, y, s, scale=1, on=True):
        """5x7 text, 6*scale px per character (lower case as upper case); returns the x after it."""
        for ch in s.upper():
            c = ord(ch) if 0x20 <= ord(ch) <= 0x5F else ord("?")
            for col, bits in enumerate(self.font[c - 0x20]):
                for row in range(7):
                    if bits >> row & 1:
                        self.rect(x + col * scale, y + row * scale, scale, scale, on)
            x += 6 * scale
        return x

    def label(self, x, y, s):
        """Text on a dark box, readable over anything."""
        self.rect(x - 1, y - 1, 6 * len(s) + 1, 9, on=False)
        return self.text(x, y, s)


def play(game, remote, stop=None, on_frame=None, fps=60):
    """Run a game (step(controls, dt) and render(screen)) on the keyboard until `stop` is set (a threading.Event)
    or the keyboard's remote screen stops (ConnectionError). on_frame(fb) sees every frame."""
    scr = Screen()
    ctrl, last = remote.show(scr.fb), time.monotonic()
    while not (stop and stop.is_set()):
        now = time.monotonic()
        game.step(ctrl, min(now - last, 0.05))
        last = now
        game.render(scr)
        if on_frame:
            on_frame(bytes(scr.fb))
        time.sleep(max(0.0, 1 / fps - (time.monotonic() - now)))  # USB manages ~110 fps; 60 is plenty
        ctrl = remote.show(scr.fb)


class Remote:
    def __init__(self, dev=None):
        self.dev = dev or Device()
        self.dev.hello()
        self.fd = self.dev._open()
        self.dev._drain(self.fd)
        r = self._msg(b"\x00")  # empty: no program listening; 47 bytes: remote.c already running
        if r == b"\x7f":        # "unknown command": firmware without program messages
            os.close(self.fd)
            raise IOError("the keyboard's firmware is too old for the remote screen - flash fw/mpk.bin")
        if not r:  # start the remote program
            if not os.path.exists(REMOTE_BIN):
                raise FileNotFoundError(f"{REMOTE_BIN} missing - build it with: make -C programs")
            os.close(self.fd)
            self.dev.run_program(open(REMOTE_BIN, "rb").read())
            self.fd = self.dev._open()
            end = time.monotonic() + 2
            while not self._msg(b"\x00"):
                if time.monotonic() > end:
                    raise ConnectionError("the remote program did not start")
                time.sleep(0.02)

    def _msg(self, data):
        return self.dev.exchange(self.fd, PROG_MSG, data)

    def show(self, fb):
        """Send a 512-byte frame to the screen; returns the controls (Controls)."""
        for c in range(4):
            r = self._msg(bytes([FRAME, c]) + pack7(fb[c * 128:c * 128 + 128]))
        return parse_controls(r)

    def close(self):
        """Back to the normal firmware."""
        try:
            self._msg(bytes([QUIT]))
        except (OSError, TimeoutError):
            pass
        os.close(self.fd)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
