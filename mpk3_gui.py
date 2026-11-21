#!/usr/bin/env python3
"""Desktop GUI for the MPK mini 3 custom firmware: every setting, applied live, saved to the keyboard on request.

usage: python3 mpk3_gui.py      (needs Tk: on Arch, sudo pacman -S tk)
"""
import json
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from mpk3_cfg import IMG_H, IMG_W, LAYOUT, SETTINGS, Device, encode, image_pixel, line_points, set_pixel, to_oled

PREVIEW = 5  # idle image editor: screen pixels per keyboard pixel

GROUPS = ["General", "Pads", "Knobs", "Joystick", "Arp & tempo", "Display"]
LIVE_DELAY_MS = 250  # send this long after the last edit, so dragging a spinbox doesn't flood the keyboard


class App:
    def __init__(self, root, device_factory=Device):
        self.root, self.factory, self.dev = root, device_factory, None
        self.values, self.vars, self.hints, self.inputs = None, {}, {}, []
        self.loading, self.pending = False, None
        root.title("MPK mini 3 custom firmware")

        top = ttk.Frame(root, padding=8)
        top.pack(fill="x")
        ttk.Button(top, text="Connect", command=self.connect).pack(side="left")
        self.live = tk.BooleanVar(value=True)
        ttk.Checkbutton(top, text="Apply changes live", variable=self.live).pack(side="left", padx=12)
        self.status = tk.StringVar(value="Not connected. Plug in the keyboard and press Connect.")
        ttk.Label(top, textvariable=self.status).pack(side="left")

        book = ttk.Notebook(root)
        book.pack(fill="both", expand=True, padx=8)
        frames = {}
        for g in GROUPS:
            frames[g] = ttk.Frame(book, padding=10)
            book.add(frames[g], text=g)
        tables = {}  # group -> (frame, row labels, column labels) for per-pad / per-knob settings
        for s in SETTINGS:
            if not s.group:
                continue  # reserved bytes
            var = tk.StringVar()
            if s.col:
                f, rows, cols = tables.setdefault(s.group, (ttk.Frame(frames[s.group], padding=(0, 12, 0, 0)), [], []))
                if not rows:
                    f.grid(row=99, column=0, columnspan=3, sticky="w")
                for lst, item in ((rows, s.row), (cols, s.col)):
                    if item not in lst:
                        lst.append(item)
                r, c = rows.index(s.row) + 1, 1 + 2 * cols.index(s.col)
                if r == 1:
                    ttk.Label(f, text=s.col, font="TkDefaultFont 9 bold").grid(row=0, column=c, sticky="w", padx=6)
                if c == 1:
                    ttk.Label(f, text=s.row).grid(row=r, column=0, sticky="w", pady=1)
                parent, row, col = f, r, c
            else:
                parent, row, col = frames[s.group], frames[s.group].grid_size()[1], 1
                ttk.Label(parent, text=s.label).grid(row=row, column=0, sticky="w", pady=3)
            if s.choices:
                w = ttk.Combobox(parent, textvariable=var, values=s.choices, state="readonly",
                                 width=max(len(c) for c in s.choices) + 2)
            else:
                w = ttk.Spinbox(parent, textvariable=var, from_=s.lo + s.shift, to=s.hi + s.shift, width=6)
            w.grid(row=row, column=col, sticky="w", padx=6)
            self.hints[s.key] = ttk.Label(parent, foreground="gray", width=6 if s.col else 0)
            self.hints[s.key].grid(row=row, column=col + 1, sticky="w")
            self.vars[s.key] = var
            self.inputs.append(w)
            var.trace_add("write", lambda *_, s=s: self.edited(s))

        # idle image (Display tab): a small pixel editor plus load / send / logo
        img = ttk.LabelFrame(frames["Display"], text="Idle image: left button draws, right button erases "
                             "(inverted every minute on the keyboard)", padding=8)
        img.grid(row=98, column=0, columnspan=3, sticky="w", pady=(12, 0))
        self.canvas = tk.Canvas(img, width=IMG_W * PREVIEW, height=IMG_H * PREVIEW, bg="black", highlightthickness=0,
                                cursor="pencil")
        self.canvas.grid(row=0, column=0, columnspan=5, pady=(0, 6))
        # one cell per keyboard pixel, created once and only recoloured, so dragging stays smooth
        self.cells = [[self.canvas.create_rectangle(x * PREVIEW, y * PREVIEW, (x + 1) * PREVIEW - 1,
                                                    (y + 1) * PREVIEW - 1, fill="black", outline="#202020")
                       for x in range(IMG_W)] for y in range(IMG_H)]
        self.logo_text = self.canvas.create_text(IMG_W * PREVIEW // 2, IMG_H * PREVIEW // 2, fill="white",
                                                 text="MPK MINI 3\ncustom firmware\n(built-in logo: draw to replace it)",
                                                 font="TkFixedFont 13 bold", justify="center")
        self.picture, self.last, self.image_note = None, None, tk.StringVar(value="")
        for button, on in (("1", True), ("3", False)):
            self.canvas.bind(f"<Button-{button}>", lambda e, on=on: self.paint(e, on))
            self.canvas.bind(f"<B{button}-Motion>", lambda e, on=on: self.paint(e, on))
            self.canvas.bind(f"<ButtonRelease-{button}>", lambda e: setattr(self, "last", None))
        for c, (text, cmd) in enumerate((("Load picture…", self.load_picture), ("Clear", self.clear_picture),
                                         ("Invert", self.invert_picture), ("Send to keyboard", self.send_picture),
                                         ("Use built-in logo", self.use_logo))):
            b = ttk.Button(img, text=text, command=cmd)
            b.grid(row=1, column=c, sticky="w", padx=(0, 6))
            if text in ("Send to keyboard", "Use built-in logo"):
                self.inputs.append(b)  # these need the keyboard; drawing works before connecting
        ttk.Label(img, textvariable=self.image_note, foreground="gray").grid(row=2, column=0, columnspan=5, sticky="w")
        self.draw_preview(None)

        bottom = ttk.Frame(root, padding=8)
        bottom.pack(fill="x")
        for text, cmd in (("Read from keyboard", self.read), ("Save to keyboard", self.save),
                          ("Defaults", self.defaults), ("Load file…", self.load_file), ("Save file…", self.save_file)):
            b = ttk.Button(bottom, text=text, command=cmd)
            b.pack(side="left", padx=(0, 6))
            self.inputs.append(b)
        self.enable(False)

    # ---- helpers ----
    def enable(self, on):
        for w in self.inputs:
            w.state(["!disabled"] if on else ["disabled"])
            if on and isinstance(w, ttk.Combobox):
                w.state(["readonly"])

    def action(self, fn, ok=None):
        """Run a device action; show problems in the status bar and a dialog instead of a traceback."""
        try:
            fn()
            if ok:
                self.status.set(ok)
            return True
        except Exception as e:  # device errors are OSError/TimeoutError/ValueError with readable messages
            self.status.set(f"Error: {e}")
            messagebox.showerror("MPK mini 3", str(e))
            return False

    def show(self, values):
        self.loading = True
        for s in SETTINGS:
            if s.key in self.vars:
                self.vars[s.key].set(s.text(values[s.key]))
                self.hint(s, values[s.key])
        self.loading = False
        self.values = dict(values)

    def hint(self, s, v):
        if s.key in self.hints:
            self.hints[s.key].config(text=(s.show(v).split(" ", 1)[1] if s.col else s.show(v)) if s.kind else "")

    # ---- edits ----
    def edited(self, s):
        if self.loading or self.values is None:
            return
        try:
            v = s.parse(self.vars[s.key].get())
        except ValueError as e:
            self.status.set(str(e))
            return
        self.values[s.key] = v
        self.hint(s, v)
        if self.live.get():
            if self.pending:
                self.root.after_cancel(self.pending)
            self.pending = self.root.after(LIVE_DELAY_MS, self.send)
        else:
            self.status.set("Changed (not sent: live is off; Save to keyboard sends and saves)")

    def send(self):
        self.pending = None
        try:
            self.dev.write(self.values)
            self.status.set("Applied to the keyboard (not saved yet)")
        except Exception as e:
            self.status.set(f"Error: {e}")

    # ---- buttons ----
    def connect(self):
        def go():
            self.dev = self.factory()
            info = self.dev.hello()
            self.show(self.dev.read())
            uploaded, data = self.dev.read_image()
            self.picture = bytearray(data) if uploaded else None  # edit the keyboard's current picture
            self.draw_preview(self.picture)
            self.image_note.set("On the keyboard: your picture" if uploaded else "On the keyboard: the built-in logo")
            self.enable(True)
            self.status.set(f"Connected: custom firmware v{info['version']}")
        self.action(go)

    # ---- idle image ----
    def draw_preview(self, data):
        """data = 512-byte picture, or None for the built-in logo (shown as text over an empty grid)."""
        for y in range(IMG_H):
            for x in range(IMG_W):
                self.canvas.itemconfig(self.cells[y][x], fill="white" if data and image_pixel(data, x, y) else "black")
        self.canvas.itemconfig(self.logo_text, state="hidden" if data is not None else "normal")

    def paint(self, event, on):
        x, y = event.x // PREVIEW, event.y // PREVIEW
        if self.picture is None:  # first stroke over the logo starts a blank picture
            self.picture = bytearray(512)
            self.draw_preview(self.picture)
        for px, py in line_points(*(self.last or (x, y)), x, y):
            if 0 <= px < IMG_W and 0 <= py < IMG_H:
                set_pixel(self.picture, px, py, on)
                self.canvas.itemconfig(self.cells[py][px], fill="white" if on else "black")
        self.last = (x, y)
        self.image_note.set("Edited - not sent yet")

    def clear_picture(self):
        self.picture = bytearray(512)
        self.draw_preview(self.picture)
        self.image_note.set("Cleared - not sent yet")

    def invert_picture(self):
        self.picture = bytearray(b ^ 0xFF for b in (self.picture or bytes(512)))
        self.draw_preview(self.picture)
        self.image_note.set("Inverted - not sent yet")

    def load_picture(self):
        path = filedialog.askopenfilename(filetypes=[("Pictures", "*.png *.gif *.ppm *.pgm")])
        if path:
            def go():
                photo = tk.PhotoImage(file=path)  # Tk reads PNG/GIF/PPM itself: no extra packages
                w, h = photo.width(), photo.height()
                gray = [[sum(photo.get(x, y)) // 3 for x in range(w)] for y in range(h)]
                self.picture = bytearray(to_oled(gray, w, h))
                self.draw_preview(self.picture)
                self.image_note.set(f"{path.rsplit('/', 1)[-1]} ({w}x{h}) - not sent yet")
            self.action(go, "Picture loaded: check the preview, then Send to keyboard")

    def send_picture(self):
        if self.picture is None:
            self.status.set("Draw or load a picture first")
            return
        def go():
            self.dev.write_image(bytes(self.picture))
            self.dev.save()
            self.image_note.set("On the keyboard: your picture")
        self.action(go, "Picture saved on the keyboard (choose Image under 'When idle, show')")

    def use_logo(self):
        def go():
            self.dev.clear_image()
            self.dev.save()
            self.picture = None
            self.draw_preview(None)
            self.image_note.set("On the keyboard: the built-in logo")
        self.action(go, "The keyboard shows its built-in logo again")

    def read(self):
        self.action(lambda: self.show(self.dev.read()), "Read from the keyboard")

    def save(self):
        def go():
            self.dev.write(self.values)
            self.dev.save()
        self.action(go, "Saved to the keyboard (kept after unplugging)")

    def defaults(self):
        if messagebox.askyesno("MPK mini 3", "Reset all settings to defaults on the keyboard? (Not saved until you "
                               "press Save to keyboard.)"):
            self.action(lambda: (self.dev.defaults(), self.show(self.dev.read())), "Defaults applied (not saved yet)")

    def save_file(self):
        path = filedialog.asksaveasfilename(defaultextension=".json", filetypes=[("MPK settings", "*.json")])
        if path:
            def go():
                with open(path, "w") as f:
                    json.dump({"layout": LAYOUT, "settings": self.values}, f, indent=1)
            self.action(go, f"Saved {path}")

    def load_file(self):
        path = filedialog.askopenfilename(filetypes=[("MPK settings", "*.json")])
        if path:
            def go():
                with open(path) as f:
                    data = json.load(f)
                if data.get("layout") != LAYOUT:
                    raise ValueError(f"{path} is for settings layout {data.get('layout')}, this is {LAYOUT}")
                values = {s.key: data["settings"][s.key] for s in SETTINGS}
                encode(values)  # validates every value before anything reaches the keyboard
                self.show(values)
                self.dev.write(values)
            self.action(go, f"Loaded {path} and applied it (not saved yet)")


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
