#!/usr/bin/env python3
"""Front panel of the MPK mini 3 emulator: its screen and LEDs, and every control to click, drag or type.

usage: python3 emu/panel.py [--socket PATH]      (default $XDG_RUNTIME_DIR/mpk3-emu/panel.sock)

Mouse: keys and pads hit harder the lower you click (Shift = 127); drag up/down or turn the wheel while holding a
pad = pressure. Knobs: wheel or drag up/down. Joystick: drag, springs back. Buttons: hold. Right-click a key, pad or
button = latch it down / let go (for combos such as PAD BANK + PROG SELECT, or chords for the arpeggiator).
Computer keyboard (like FL Studio's typing keyboard): Z S X D C V G B H N J M , = lowest octave,
Q 2 W 3 E R 5 T 6 Y 7 U I = the next one up; velocity from the slider.
"""
import argparse
import math
import time
import tkinter as tk

from emu_client import Panel, default_socket, pixel

BG, CARD, INK, DIM, ACCENT = "#15171c", "#22252d", "#e8e8ea", "#9aa0aa", "#ffb347"  # mpk3_market's colours
OLED_INK = (190, 220, 255)   # lit OLED pixel at full contrast
LED_RED, LED_GREEN, PAD_LIT = (255, 70, 40), (60, 255, 90), (255, 60, 50)
OFF, PAD_OFF, PRESSED = (50, 53, 62), (42, 45, 53), "#5a5f6c"
OFF_HEX = "#32353e"
W, H = 1400, 520

BUTTONS = ["PROG SELECT", "CC", "PROG CHANGE", "PAD BANK", "ARP", "TAP TEMPO", "OCT +", "OCT −", "FULL LEVEL",
           "NOTE REPEAT"]                                  # firmware/NOTES.md numbering
BUTTON_LED = [4, 3, 2, None, 16, 17, 18, 19, 20, 21]      # PAD BANK: red LED 0 (bank A) + green LED 1 (bank B)
BUTTON_AT = {4: (16, 196), 5: (120, 196), 7: (16, 236), 6: (120, 236), 8: (16, 276), 9: (120, 276),
             0: (293, 196), 1: (399, 196), 2: (505, 196), 3: (611, 196)}
BTN_W, BTN_H = 96, 30
UNUSED_LEDS = [5, 6, 7, 22, 23]                           # driven by the shift registers, not seen on the panel
TYPING = {**{c: i for i, c in enumerate("zsxdcvgbhnjm")}, "comma": 12,
          **{c: 12 + i for i, c in enumerate("q2w3er5t6y7ui")}}
WHITE = [0, 2, 4, 5, 7, 9, 11]                            # semitones of the white keys
KEYS_X, KEYS_Y, KEYS_H, BLACK_H = 236, 330, 180, 110
WHITE_W = (W - 16 - KEYS_X) / 15
OLED_X, OLED_Y = 244, 32
JOY = (116, 99, 75)                                       # centre x, y and half size of the joystick box
PAD_X, PAD_Y, PAD_S = 788, 24, 78
KNOB_X, KNOB_Y, KNOB_R = 1172, (66, 160), 22


def mix(a, b, t):
    return "#%02x%02x%02x" % tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def glow(level):
    return (level / 255) ** 0.5  # dim PWM levels (button LEDs run at 4/32) stay visible, as they do to the eye


def oled_ppm(on, contrast, invert, fb):
    """The 128x32 picture as a 512x128 PPM, each pixel a 3x3 dot with a dark gap like the real panel."""
    dark = b"\0\0\0"
    lit = bytes(round(c * (0.35 + 0.65 * contrast / 255)) for c in OLED_INK)
    dot, gap = lit * 3 + dark, dark * 4
    rows = []
    for y in range(32):
        row = b"".join(dot if on and pixel(fb, x, y) != invert else gap for x in range(128))
        rows.append(row * 3 + dark * 512)
    return b"P6 512 128 255\n" + b"".join(rows)


class FrontPanel:
    def __init__(self, root, path=None):
        self.root, self.path, self.client = root, path or default_socket(), None
        self.holders = {}        # (kind, i) -> sources holding it down: "mouse", "latch" or a typing keysym
        self.grab = None         # (kind, i, last y) of what the left mouse button holds
        self.force, self.angle = [0] * 8, [0] * 8
        self.joy_xy, self.pedal_down, self.retry_at, self.new_status = (0, 0), False, 0, None
        self.repeat = {}         # keysym -> pending release (filters X11 key autorepeat)
        self.shown_oled = self.shown_leds = self.oled_img = None
        root.title("MPK mini 3 emulator")
        root.configure(bg=BG)
        self.c = c = tk.Canvas(root, width=W, height=H, bg=BG, highlightthickness=0)
        c.pack(padx=8, pady=(8, 0))
        self.items, self.geom, self.leds = {}, {}, {}
        self.build()

        bar = tk.Frame(root, bg=BG)
        bar.pack(fill="x", padx=16, pady=4)
        tk.Label(bar, text="Typing velocity", fg=DIM, bg=BG).pack(side="left")
        self.velocity = tk.Scale(bar, from_=1, to=127, orient="horizontal", length=200, bg=BG, fg=INK,
                                 troughcolor=CARD, highlightthickness=0, takefocus=0)
        self.velocity.set(100)
        self.velocity.pack(side="left", padx=8)
        tk.Label(bar, text="Typing keys: Z S X D C V G B H N J M ,  and  Q 2 W 3 E R 5 T 6 Y 7 U I.   Click lower = "
                 "harder, Shift = 127, right-click = latch, wheel = knobs / pad pressure.", fg=DIM, bg=BG).pack(
            side="left", padx=8)
        self.status = tk.StringVar(value="Starting")
        tk.Label(root, textvariable=self.status, fg=DIM, bg=BG, anchor="w").pack(fill="x", padx=16, pady=(0, 8))

        c.bind("<ButtonPress-1>", self.press)
        c.bind("<B1-Motion>", self.drag)
        c.bind("<ButtonRelease-1>", self.release)
        c.bind("<ButtonPress-3>", self.latch)
        c.bind("<Button-4>", lambda e: self.wheel(e, 1))
        c.bind("<Button-5>", lambda e: self.wheel(e, -1))
        c.bind("<MouseWheel>", lambda e: self.wheel(e, 1 if e.delta > 0 else -1))
        root.bind_all("<KeyPress>", self.key_down)
        root.bind_all("<KeyRelease>", self.key_up)
        root.bind("<FocusOut>", lambda e: self.let_go(lambda src: src not in ("mouse", "latch")))
        root.protocol("WM_DELETE_WINDOW", self.close)
        self.tick()

    # ---- drawing ----
    def shape(self, kind, i, make, *coords, **kw):
        item = make(*coords, tags=(f"{kind}:{i}",), **kw)
        self.items.setdefault((kind, i), item)  # the first shape of a control is the one that gets recoloured
        self.geom.setdefault((kind, i), coords)
        return item

    def build(self):
        c = self.c
        x, y, r = JOY
        self.shape("joy", 0, c.create_rectangle, x - r, y - r, x + r, y + r, fill=CARD, outline="#444")
        c.create_line(x - r, y, x + r, y, fill="#333", tags="joy:0")
        c.create_line(x, y - r, x, y + r, fill="#333", tags="joy:0")
        self.joy_dot = c.create_oval(x - 12, y - 12, x + 12, y + 12, fill=DIM, outline="", tags="joy:0")
        self.joy_text = c.create_text(x, y + r + 10, text="JOYSTICK", fill=DIM, font="TkDefaultFont 8")
        for b, (bx, by) in BUTTON_AT.items():
            self.shape("button", b, c.create_rectangle, bx, by, bx + BTN_W, by + BTN_H, fill=OFF_HEX,
                       outline="", width=2)
            self.leds[b] = c.create_oval(bx + 7, by + 11, bx + 15, by + 19, fill=OFF_HEX, outline="",
                                         tags=f"button:{b}")
            c.create_text(bx + 56, by + BTN_H / 2, text=BUTTONS[b], fill=INK, font="TkDefaultFont 7 bold",
                          tags=f"button:{b}")
        for kind, label, bx in (("pedal", "SUSTAIN", 16), ("power", "POWER CYCLE", 120)):
            self.shape(kind, 0, c.create_rectangle, bx, 340, bx + BTN_W, 340 + BTN_H, fill=OFF_HEX,
                       outline="", width=2)
            c.create_text(bx + BTN_W / 2, 340 + BTN_H / 2, text=label, fill=INK, font="TkDefaultFont 7 bold",
                          tags=f"{kind}:0")
        c.create_text(16, 396, text="LEDs 5 6 7 22 23 (not on the panel)", fill=DIM, anchor="w",
                      font="TkDefaultFont 7")
        for n, led in enumerate(UNUSED_LEDS):
            self.leds[f"u{led}"] = c.create_oval(20 + 18 * n, 408, 30 + 18 * n, 418, fill=OFF_HEX,
                                                 outline="")

        c.create_rectangle(OLED_X - 8, OLED_Y - 8, OLED_X + 520, OLED_Y + 136, fill="black", outline="#444")
        self.oled_item = c.create_image(OLED_X, OLED_Y, anchor="nw")
        self.oled_text = c.create_text(OLED_X + 256, OLED_Y + 64, fill=DIM, font="TkDefaultFont 11", justify="center")

        for p in range(8):  # Akai numbering: bottom row pads 1-4, top row 5-8
            px, py = PAD_X + (p % 4) * (PAD_S + 8), PAD_Y + (0 if p >= 4 else PAD_S + 10)
            self.shape("pad", p, c.create_rectangle, px, py, px + PAD_S, py + PAD_S, fill=mix(PAD_OFF, PAD_LIT, 0),
                       outline="#444", width=1)
            c.create_text(px + 8, py + 8, text=str(p + 1), fill=DIM, anchor="nw", tags=f"pad:{p}")
            self.items["force", p] = c.create_text(px + PAD_S / 2, py + PAD_S - 12, fill=INK, tags=f"pad:{p}",
                                                   font="TkDefaultFont 7")
        for k in range(8):
            kx, ky = KNOB_X + (k % 4) * 60, KNOB_Y[k // 4]
            self.shape("knob", k, c.create_oval, kx - KNOB_R, ky - KNOB_R, kx + KNOB_R, ky + KNOB_R, fill=CARD,
                       outline="#555", width=2)
            self.items["mark", k] = c.create_line(kx, ky, kx, ky - KNOB_R, fill=INK, width=3, tags=f"knob:{k}")
            c.create_text(kx, ky + KNOB_R + 10, text=f"K{k + 1}", fill=DIM, font="TkDefaultFont 8")
        c.create_text(W - 16, 270, text="MPK mini 3", fill=INK, anchor="e", font="TkDefaultFont 20 bold")
        c.create_text(W - 16, 298, text="emulator front panel", fill=DIM, anchor="e")

        labels = {}
        for sym, k in TYPING.items():
            labels.setdefault(k, []).append("," if sym == "comma" else sym.upper())
        for black in (False, True):  # white keys first, so the black ones are on top
            for k in range(25):
                octave, s = divmod(k, 12)
                if (s not in WHITE) != black:
                    continue
                if black:
                    mid = KEYS_X + (octave * 7 + WHITE.index(s - 1) + 1) * WHITE_W
                    x0, x1, y1, fill, ink = mid - WHITE_W * 0.3, mid + WHITE_W * 0.3, KEYS_Y + BLACK_H, "#111", DIM
                else:
                    x0 = KEYS_X + (octave * 7 + WHITE.index(s)) * WHITE_W
                    x1, y1, fill, ink = x0 + WHITE_W - 2, KEYS_Y + KEYS_H, "#f4f4f2", "#888"
                self.shape("key", k, c.create_rectangle, x0, KEYS_Y, x1, y1, fill=fill, outline="#000")
                c.create_text((x0 + x1) / 2, y1 - 12, text=" ".join(labels[k]), fill=ink, font="TkDefaultFont 8",
                              tags=f"key:{k}")
        self.set_offline("Connecting…")

    def redraw(self, kind, i):
        down = bool(self.holders.get((kind, i)))
        latched = "latch" in self.holders.get((kind, i), ())
        item = self.items[kind, i]
        if kind == "key":
            self.c.itemconfig(item, fill=ACCENT if down else "#111" if i % 12 not in WHITE else "#f4f4f2")
        elif kind == "pad":
            self.c.itemconfig(item, outline=ACCENT if down else "#444", width=3 if down else 1)
            self.c.itemconfig(self.items["force", i], text=f"pressure {self.force[i]}" if down else "")
        elif kind in ("button", "pedal"):
            down = down or (kind == "pedal" and self.pedal_down)
            self.c.itemconfig(item, fill=PRESSED if down else OFF_HEX, outline=ACCENT if latched or (
                kind == "pedal" and down) else "")

    def draw_leds(self, lv):
        for b, led in enumerate(BUTTON_LED):
            if led is None:  # PAD BANK: red and green halves of one light
                col = tuple(min(255, o + (r - o) * glow(lv[0]) + (g - o) * glow(lv[1]))
                            for o, r, g in zip(OFF, LED_RED, LED_GREEN))
                self.c.itemconfig(self.leds[b], fill=mix(col, col, 0))
            else:
                self.c.itemconfig(self.leds[b], fill=mix(OFF, LED_RED, glow(lv[led])))
        for p in range(8):
            self.c.itemconfig(self.items["pad", p], fill=mix(PAD_OFF, PAD_LIT, glow(lv[8 + p])))
        for led in UNUSED_LEDS:
            self.c.itemconfig(self.leds[f"u{led}"], fill=mix(OFF, LED_RED, glow(lv[led])))

    def set_offline(self, text):
        self.c.itemconfig(self.oled_text, text=text)
        self.c.itemconfig(self.oled_item, image="")
        self.draw_leds(bytes(24))
        self.shown_oled = self.shown_leds = None

    # ---- talking to the emulator ----
    def send(self, method, *args):
        if self.client and self.client.connected:
            try:
                getattr(self.client, method)(*args)
            except ConnectionError:
                pass  # tick() sees the dropped connection and reconnects

    def connect(self):
        try:
            self.client = Panel(self.path, timeout=0.5, on_status=lambda t: setattr(self, "new_status", t))
        except ConnectionError:
            self.retry_at = time.monotonic() + 1
            self.status.set(f"Emulator not running: start it (emu/mpk3emu FLASHFILE --image fw/mpk.bin); "
                            f"waiting for {self.path}")
            self.c.itemconfig(self.oled_text, text="Emulator not running\nwaiting for it to start…")
            return
        self.status.set(f"Connected to {self.path}")
        self.c.itemconfig(self.oled_text, text="")
        self.send("pedal", self.pedal_down)  # a restarted emulator starts with the pedal up

    def tick(self):
        cl = self.client
        if cl is not None and not cl.connected:
            self.client = None
            self.holders, self.grab = {}, None  # the emulator that held them is gone
            for kind, i in self.geom:
                self.redraw(kind, i)
            self.set_offline("Emulator stopped\nreconnecting…")
            self.status.set("Emulator stopped - reconnecting")
        elif cl is None and time.monotonic() >= self.retry_at:
            self.connect()
        elif cl is not None:
            if cl.oled is not self.shown_oled:
                self.shown_oled = cl.oled
                self.oled_img = tk.PhotoImage(data=oled_ppm(*cl.oled))
                self.c.itemconfig(self.oled_item, image=self.oled_img)
            if cl.leds is not self.shown_leds:
                self.shown_leds = cl.leds
                self.draw_leds(cl.leds)
        if self.new_status:
            self.status.set(f"Emulator: {self.new_status}")
            self.new_status = None
        self.root.after(30, self.tick)

    def close(self):
        self.let_go(lambda src: True)
        if self.client:
            self.client.close()
        self.root.destroy()

    # ---- holding things down (a key/pad/button stays down while any source holds it) ----
    def hold(self, kind, i, src, value):
        h = self.holders.setdefault((kind, i), set())
        if not h:
            if kind == "pad":
                self.force[i] = value
            self.send(kind, i, value)
        h.add(src)
        self.redraw(kind, i)

    def unhold(self, kind, i, src):
        h = self.holders.get((kind, i), set())
        if src in h:
            h.discard(src)
            if not h:
                self.send(kind, i, 0)
        self.redraw(kind, i)

    def let_go(self, which):
        for (kind, i), h in list(self.holders.items()):
            for src in [s for s in h if which(s)]:
                self.unhold(kind, i, src)

    def target(self, e):
        for item in reversed(self.c.find_overlapping(e.x, e.y, e.x, e.y)):  # topmost first: black keys
            for tag in self.c.gettags(item):
                if ":" in tag:
                    kind, i = tag.split(":")
                    return kind, int(i)
        return None, None

    def strength(self, kind, i, y):
        """Velocity / force 1-127 from how low on the key or pad the click was."""
        _, y0, _, y1 = self.geom[kind, i]
        return max(1, min(127, round(1 + 126 * (y - y0) / (y1 - y0))))

    # ---- mouse ----
    def press(self, e):
        kind, i = self.target(e)
        if kind is None:
            return
        self.grab = (kind, i, e.y)
        if kind in ("key", "pad"):
            self.hold(kind, i, "mouse", 127 if e.state & 1 else self.strength(kind, i, e.y))
        elif kind == "button":
            self.hold(kind, i, "mouse", 1)
        elif kind == "joy":
            self.move_joy(e)
        elif kind == "pedal":
            self.pedal_down = not self.pedal_down
            self.send("pedal", self.pedal_down)
            self.redraw("pedal", 0)
        elif kind == "power":
            self.send("power")
            self.status.set("Power cycle (held buttons stay held: latch PAD BANK + PROG SELECT for update mode)")

    def drag(self, e):
        if not self.grab:
            return
        kind, i, y0 = self.grab
        if kind == "pad" and self.holders.get(("pad", i)):
            self.set_force(i, self.force[i] + y0 - e.y)
        elif kind == "knob":
            self.turn(i, 2 * (y0 - e.y))
        elif kind == "joy":
            self.move_joy(e)
        self.grab = (kind, i, e.y)

    def release(self, e):
        if not self.grab:
            return
        kind, i, _ = self.grab
        self.grab = None
        if kind in ("key", "pad", "button"):
            self.unhold(kind, i, "mouse")
        elif kind == "joy":
            self.set_joy(0, 0)

    def latch(self, e):
        kind, i = self.target(e)
        if kind not in ("key", "pad", "button"):
            return
        if "latch" in self.holders.get((kind, i), ()):
            self.unhold(kind, i, "latch")
        else:
            self.hold(kind, i, "latch", 1 if kind == "button" else self.strength(kind, i, e.y))

    def wheel(self, e, step):
        kind, i = (self.grab or (None, None))[:2]
        if kind != "pad":
            kind, i = self.target(e)
        if kind == "knob":
            self.turn(i, 10 * step)
        elif kind == "pad" and self.holders.get(("pad", i)):
            self.set_force(i, self.force[i] + 8 * step)

    def set_force(self, p, f):
        f = max(1, min(127, f))
        if f != self.force[p]:
            self.force[p] = f
            self.send("pad", p, f)
            self.redraw("pad", p)

    def turn(self, k, degrees):
        if not degrees:
            return
        self.send("knob", k, degrees)
        self.angle[k] += degrees
        kx, ky, a = KNOB_X + (k % 4) * 60, KNOB_Y[k // 4], math.radians(self.angle[k])
        self.c.coords(self.items["mark", k], kx, ky, kx + KNOB_R * math.sin(a), ky - KNOB_R * math.cos(a))

    def move_joy(self, e):
        x, y, r = JOY
        self.set_joy(round(max(-1, min(1, (e.x - x) / r)) * 1000), round(max(-1, min(1, (y - e.y) / r)) * 1000))

    def set_joy(self, jx, jy):
        if (jx, jy) == self.joy_xy:
            return
        self.joy_xy = (jx, jy)
        self.send("joy", jx, jy)
        x, y, r = JOY
        dx, dy = jx * r / 1000, -jy * r / 1000
        self.c.coords(self.joy_dot, x + dx - 12, y + dy - 12, x + dx + 12, y + dy + 12)
        self.c.itemconfig(self.joy_text, text=f"JOYSTICK {jx:+} {jy:+}" if (jx, jy) != (0, 0) else "JOYSTICK")

    # ---- computer keyboard ----
    def key_down(self, e):
        sym = e.keysym.lower()
        if sym not in TYPING:
            return
        if sym in self.repeat:  # X11 autorepeat sends release+press pairs: the key never went up
            self.root.after_cancel(self.repeat.pop(sym))
            return
        self.hold("key", TYPING[sym], sym, self.velocity.get())

    def key_up(self, e):
        sym = e.keysym.lower()
        if sym in TYPING and sym not in self.repeat:
            self.repeat[sym] = self.root.after(30, lambda: (self.repeat.pop(sym, None),
                                                            self.unhold("key", TYPING[sym], sym)))


def main():
    ap = argparse.ArgumentParser(description="Front panel of the MPK mini 3 emulator")
    ap.add_argument("--socket", help=f"panel socket (default {default_socket()})")
    args = ap.parse_args()
    root = tk.Tk()
    FrontPanel(root, args.socket)
    root.mainloop()


if __name__ == "__main__":
    main()
