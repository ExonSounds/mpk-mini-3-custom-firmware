#!/usr/bin/env python3
"""Desktop GUI for the MPK mini 3 custom firmware: every setting, applied live, saved to the keyboard on request.

usage: python3 mpk3_gui.py      (needs Tk: on Arch, sudo pacman -S tk)
"""
import json
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from mpk3_cfg import LAYOUT, SETTINGS, Device, encode

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
            self.enable(True)
            self.status.set(f"Connected: custom firmware v{info['version']}")
        self.action(go)

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
