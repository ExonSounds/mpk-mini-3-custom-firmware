"""Joystick Mouse: the keyboard's joystick moves this computer's mouse pointer; PAD 8 left-clicks, PAD 4 right-clicks.

The pointer comes from a virtual mouse made with the kernel's uinput (/dev/uinput), so it works on Wayland and X11
alike; it disappears again when the app is unloaded.
"""
import fcntl
import os
import struct
import time

LEFT_PAD, RIGHT_PAD = 7, 3           # PAD 8, PAD 4
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
            "the pads click. Hold a pad to drag.")
    CONTROLS = "Joystick X/Y: move the pointer   PAD 8: left click   PAD 4: right click"

    def __init__(self, mouse=None):
        self.mouse = mouse if mouse is not None else VirtualMouse()
        self.fx = self.fy = 0.0                    # sub-pixel movement carried to the next frame
        self.joy, self.left, self.right = (0, 0), False, False

    def step(self, ctrl, dt):
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
