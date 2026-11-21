"""Joystick Mouse: the keyboard's joystick moves this computer's mouse pointer; PAD 8 left-clicks, PAD 4 right-clicks.

The pointer comes from a virtual mouse made with the kernel's uinput (/dev/uinput), so it works on Wayland and X11
alike; it disappears again when the app is unloaded.
"""
import fcntl
import os
import struct
import time

import numpy as np

from games.visualiser import DITHER, pack
from mpk3_remote import pressed

LEFT_PAD, RIGHT_PAD = 7, 3           # PAD 8, PAD 4
VIEW_PAD, ZOOM_PAD, SHADE_PAD = 0, 1, 2  # PAD 1: magnifier on/off, PAD 2: next zoom, PAD 3: dither / PWM
SHADES = ("DITHER", "PWM", "DITHER+PWM")  # dot patterns (16 greys), flicker (5), or both (64)
PWM_STEPS = (0.125, 0.625, 0.375, 0.875)  # one threshold per frame; this order spreads the on frames out
# both: the 4x4 pattern picks one of 16 bands per pixel and the frame a quarter step inside it, so a shade is
# exact to 1/64 while only the pixels whose band it falls in flicker (about 1 in 16), the rest stay steady
COMBINED = [(np.round(DITHER * 16 - 0.5) * 4 + step + 0.5) / 64 for step in (0, 2, 1, 3)]
PWM_FPS = 120                        # ask for as many frames as USB gives (~110): the faster, the steadier
ZOOMS = (1, 2, 4)                    # screen pixels per keyboard pixel ... keyboard pixels per screen pixel
DEAD, TOP_SPEED = 80, 1400           # joystick dead zone (of 1000) and pointer speed at full tilt, px per second
EV_SYN, EV_KEY, EV_REL = 0, 1, 2
BTN_LEFT, BTN_RIGHT, REL_X, REL_Y = 0x110, 0x111, 0, 1
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_RELBIT = 0x40045564, 0x40045565, 0x40045566  # _IOW('U', 100..102, int)
UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502


class VirtualMouse:
    """A two-button relative mouse made with uinput."""

    def __init__(self, path="/dev/uinput"):
        try:
            self.fd = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
        except PermissionError:
            raise PermissionError(f"{path} is not writable for you, so no virtual mouse can be made (it needs an "
                                  "ACL or udev rule giving your user access)") from None
        for ev in (EV_SYN, EV_KEY, EV_REL):
            fcntl.ioctl(self.fd, UI_SET_EVBIT, ev)
        for key in (BTN_LEFT, BTN_RIGHT):
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, key)
        for rel in (REL_X, REL_Y):
            fcntl.ioctl(self.fd, UI_SET_RELBIT, rel)
        # struct uinput_user_dev: name[80], id (bus, vendor, product, version), ff_effects_max, abs arrays 4 x 64
        os.write(self.fd, struct.pack("80sHHHHi256i", b"MPK mini 3 joystick mouse", 0x03, 0x1209, 0x0001, 1, 0,
                                      *[0] * 256))
        fcntl.ioctl(self.fd, UI_DEV_CREATE)
        self.held = set()

    def _emit(self, *events):
        t = time.time()
        sec, usec = int(t), int(t % 1 * 1e6)
        os.write(self.fd, b"".join(struct.pack("llHHi", sec, usec, *e) for e in events + ((EV_SYN, 0, 0),)))

    def move(self, dx, dy):
        self._emit((EV_REL, REL_X, dx), (EV_REL, REL_Y, dy))

    def button(self, code, down):
        if down != (code in self.held):
            (self.held.add if down else self.held.discard)(code)
            self._emit((EV_KEY, code, int(down)))

    def close(self):
        for code in list(self.held):  # never leave a button stuck down
            self.button(code, False)
        fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        os.close(self.fd)


def speed(v):
    """Joystick -1000..1000 -> px per second: nothing in the dead zone, then a curve that is fine near the middle."""
    a = max(0.0, (abs(v) - DEAD) / (1000 - DEAD))
    return (1 if v > 0 else -1) * a * a * TOP_SPEED


class JoyMouse:
    TITLE = "Joystick Mouse"
    INFO = ("Use the keyboard as this computer's mouse: the joystick moves the pointer (further = faster), "
            "the pads click. Hold a pad to drag. A magnifier shows the screen around the pointer.")
    CONTROLS = ("Joystick X/Y: move the pointer   PAD 8: left click   PAD 4: right click   "
                "PAD 1: magnifier on/off   PAD 2: zoom 1x / 2x / 4x   PAD 3: grey as dots, flicker (PWM) or both")

    def __init__(self, mouse=None, tracker=None, grab=None):
        self.mouse = mouse if mouse is not None else VirtualMouse()
        self.fx = self.fy = 0.0                    # sub-pixel movement carried to the next frame
        self.joy, self.left, self.right = (0, 0), False, False
        self.pads, self.magnify, self.zoom = (0,) * 8, False, 1     # zoom: index into ZOOMS
        self.tracker, self.grab, self.shot, self.error = tracker, grab, None, None
        self.note, self.note_t = "", 0.0
        self.shade, self.frame = 0, 0                      # SHADES index; frame counts PWM steps

    @property
    def fps(self):                                 # play() reads this every frame
        return PWM_FPS if self.magnify and self._flickers() else 60

    def _flickers(self):
        return SHADES[self.shade] != "DITHER"

    def _magnifier(self):
        """Follow the pointer and grab the screen around it (KWin: games/kwin.py, loaded only when needed)."""
        if self.tracker is None and self.error is None:
            try:
                from games import kwin
                self.tracker, self.grab = kwin.PointerTracker(), self.grab or kwin.capture
            except (ImportError, OSError, ValueError) as e:
                self.error = str(e)
                return
        pos = self.tracker.poll() if self.tracker else None
        z = ZOOMS[self.zoom]
        w, h = 128 // z, 32 // z
        # flicker modes: one capture per 4-frame cycle leaves more time for frames - but a new zoom needs one now
        mid_cycle = (self._flickers() and self.frame != len(PWM_STEPS) - 1 and self.shot is not None
                     and self.shot.shape == (h, w))
        if pos and not mid_cycle:
            self.shot = self.grab(pos[0] - w // 2, pos[1] - h // 2, w, h)

    def step(self, ctrl, dt):
        for p in pressed(self.pads, ctrl.pads):
            if p == VIEW_PAD:
                self.magnify = not self.magnify
                self.note = f"MAGNIFIER {ZOOMS[self.zoom]}X" if self.magnify else "MOUSE"
                self.note_t = 1.0
            elif p == ZOOM_PAD:
                self.zoom = (self.zoom + 1) % len(ZOOMS)
                self.note, self.note_t = f"ZOOM {ZOOMS[self.zoom]}X", 1.0
            elif p == SHADE_PAD:
                self.shade = (self.shade + 1) % len(SHADES)
                self.note, self.note_t = f"SHADES: {SHADES[self.shade]}", 1.0
        self.pads = ctrl.pads
        self.note_t = max(0.0, self.note_t - dt)
        if self.magnify:
            self._magnifier()
        self.joy = (ctrl.joy_x, ctrl.joy_y)
        self.fx += speed(ctrl.joy_x) * dt
        self.fy -= speed(ctrl.joy_y) * dt          # joystick up = pointer up (screen y grows downwards)
        dx, dy = int(self.fx), int(self.fy)
        if dx or dy:
            self.fx, self.fy = self.fx - dx, self.fy - dy
            self.mouse.move(dx, dy)
        self.left, self.right = ctrl.pads[LEFT_PAD] > 0, ctrl.pads[RIGHT_PAD] > 0
        self.mouse.button(BTN_LEFT, self.left)
        self.mouse.button(BTN_RIGHT, self.right)

    def render(self, scr):
        if self.magnify:
            self._render_magnifier(scr)
        else:
            self._render_mouse(scr)
        if self.note_t:
            scr.label(1, 1, self.note)

    def _render_magnifier(self, scr):
        if self.shot is None:
            scr.clear()
            scr.text(0, 0, "MAGNIFIER")
            scr.text(0, 12, "NEEDS KDE PLASMA" if self.error else "WAITING FOR")
            scr.text(0, 21, "(KWIN)" if self.error else "THE POINTER...")
            return
        z = 128 // self.shot.shape[1]              # from the picture itself, in case the zoom just changed
        img = np.repeat(np.repeat(self.shot, z, axis=0), z, axis=1)[:32, :128]
        lo, hi = float(img.min()), float(img.max())
        img = (img - lo) / (hi - lo) if hi - lo > 0.04 else np.full_like(img, 0.5 if hi > 0.5 else 0.0)
        if self._flickers():                       # a different threshold each frame
            self.frame = (self.frame + 1) % len(PWM_STEPS)
            pwm = SHADES[self.shade] == "PWM"
            scr.fb[:] = pack(img > (PWM_STEPS[self.frame] if pwm else COMBINED[self.frame]))
        else:
            scr.fb[:] = pack(img > DITHER)         # contrast stretched, then dithered: text stays crisp

    def _render_mouse(self, scr):
        scr.clear()
        scr.text(0, 0, "MOUSE")
        scr.text(0, 16, "4=R")
        scr.text(0, 25, "8=L")
        for x in range(34, 63, 2):                 # the joystick's square, with its position as a dot
            scr.px(x, 2)
            scr.px(x, 30)
        for y in range(2, 31, 2):
            scr.px(34, y)
            scr.px(62, y)
        scr.rect(48 + self.joy[0] * 13 // 1000 - 1, 16 - self.joy[1] * 13 // 1000 - 1, 3, 3)
        for name, on, x in (("R", self.right, 74), ("L", self.left, 100)):  # buttons: solid while held
            scr.rect(x, 6, 22, 20)
            if not on:
                scr.rect(x + 1, 7, 20, 18, on=False)
            scr.text(x + 6, 9, name, 2, on=not on)

    def close(self):
        self.mouse.close()
        if self.tracker:
            self.tracker.close()                   # takes the KWin script away again

    @classmethod
    def preview(cls, scr):
        g = cls(mouse=NoMouse())
        g.joy, g.left = (600, 300), True
        g.render(scr)


class NoMouse:
    """Stands in for the virtual mouse in previews and tests; records what would have been sent."""

    def __init__(self):
        self.moves, self.buttons, self.held = [], [], set()

    def move(self, dx, dy):
        self.moves.append((dx, dy))

    def button(self, code, down):  # like VirtualMouse: only changes count
        if down != (code in self.held):
            (self.held.add if down else self.held.discard)(code)
            self.buttons.append((code, down))

    def close(self):
        pass
