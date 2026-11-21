#!/usr/bin/env python3
"""Terminal UI for configuring an Akai MPK mini 3 over USB.

Before the first write of a session, all programs are backed up to
backups/<timestamp>/progN.syx. Restore one with:  cat progN.syx > /dev/snd/midiC<card>D0
"""
import curses
import os
import sys
import time

from mpk3 import FIELDS, MPK, write_msg

HELP = "Up/Down move  Left/Right -/+1  [ ] -/+10  Enter edit  0-8 program (0=live)  w write  r reload  q quit"
BACKUP_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "backups")


class App:
    def __init__(self, scr, dev, prog, payload):
        self.scr, self.dev = scr, dev
        self.prog, self.payload, self.orig = prog, payload, bytes(payload)
        self.sel = self.top = 0
        self.msg = ""
        self.backup_path = None

    @property
    def dirty(self):
        return bytes(self.payload) != self.orig

    def load(self, prog):
        self.payload = self.dev.read(prog)
        self.orig, self.prog = bytes(self.payload), prog
        self.msg = f"Loaded {self.title()}"

    def title(self):
        return "live program (RAM)" if self.prog == 0 else f"program {self.prog}"

    def backup(self):
        path = os.path.join(BACKUP_DIR, time.strftime("%Y%m%d-%H%M%S"))
        os.makedirs(path)
        for p in range(9):
            with open(os.path.join(path, f"prog{p}.syx"), "wb") as f:
                f.write(write_msg(p, self.dev.read(p)))
        return path

    def write(self):
        if self.backup_path is None:
            self.backup_path = self.backup()
        self.dev.write(self.prog, self.payload)
        self.orig = bytes(self.payload)
        self.msg = f"Wrote {self.title()} (verified). Backup: {os.path.relpath(self.backup_path)}"

    # ---- drawing / input ----

    def draw(self):
        s = self.scr
        s.erase()
        h, w = s.getmaxyx()
        if h < 6 or w < 50:
            s.addnstr(0, 0, "Terminal too small", w - 1)
            return
        head = f" MPK mini 3 | {self.title()} | {FIELDS[0].show(self.payload)}"
        s.addnstr(0, 0, (head + ("  [modified - press w to write]" if self.dirty else "")).ljust(w), w,
                  curses.A_REVERSE | curses.A_BOLD)
        rows = h - 3
        self.top = min(max(self.top, self.sel - rows + 1), self.sel)
        for y, i in enumerate(range(self.top, min(len(FIELDS), self.top + rows)), 1):
            f = FIELDS[i]
            new_sec = i == 0 or FIELDS[i - 1].section != f.section
            mark = " *" if f.get(self.payload) != f.get(self.orig) else ""
            line = f" {f.section if new_sec or i == self.top else '':<12} {f.label:<18} {f.show(self.payload)}{mark}"
            attr = curses.A_REVERSE if i == self.sel else (curses.A_BOLD if new_sec else 0)
            s.addnstr(y, 0, line.ljust(w), w, attr)
        s.addnstr(h - 2, 0, self.msg, w - 1, curses.A_BOLD)
        s.addnstr(h - 1, 0, HELP, w - 1, curses.A_DIM)

    def prompt(self, text):
        h, w = self.scr.getmaxyx()
        self.scr.move(h - 2, 0)
        self.scr.clrtoeol()
        self.scr.addnstr(h - 2, 0, text, w - 1)
        curses.echo()
        _cursor(1)
        try:
            return self.scr.getstr(h - 2, min(len(text), w - 2), 40).decode(errors="replace").strip()
        finally:
            curses.noecho()
            _cursor(0)

    def confirm(self, text):
        return not self.dirty or self.prompt(text + " (y/N) ").lower() == "y"

    def run(self):
        _cursor(0)
        steps = {curses.KEY_LEFT: -1, curses.KEY_RIGHT: 1, ord("["): -10, ord("]"): 10}
        while True:
            self.draw()
            k = self.scr.getch()
            h = self.scr.getmaxyx()[0]
            f = FIELDS[self.sel]
            self.msg = ""
            try:
                if k in (curses.KEY_UP, ord("k")):
                    self.sel = max(0, self.sel - 1)
                elif k in (curses.KEY_DOWN, ord("j")):
                    self.sel = min(len(FIELDS) - 1, self.sel + 1)
                elif k == curses.KEY_PPAGE:
                    self.sel = max(0, self.sel - (h - 3))
                elif k == curses.KEY_NPAGE:
                    self.sel = min(len(FIELDS) - 1, self.sel + (h - 3))
                elif k == curses.KEY_HOME:
                    self.sel = 0
                elif k == curses.KEY_END:
                    self.sel = len(FIELDS) - 1
                elif k in steps and not f.text:
                    f.set(self.payload, max(f.lo, min(f.hi, f.get(self.payload) + steps[k])))
                elif k in (10, 13, curses.KEY_ENTER):
                    s = self.prompt(f"{f.section} / {f.label} [{f.show(self.payload)}]: ")
                    if s:  # empty input cancels
                        f.set(self.payload, f.parse(s))
                elif ord("0") <= k <= ord("8"):
                    if self.confirm("Discard unwritten changes?"):
                        self.load(k - ord("0"))
                elif k == ord("r"):
                    if self.confirm("Discard unwritten changes?"):
                        self.load(self.prog)
                elif k == ord("w"):
                    self.write()
                elif k == ord("q"):
                    if self.confirm("Quit without writing?"):
                        return
            except (ValueError, OSError) as e:
                self.msg = f"Error: {e}"


def _cursor(v):
    try:
        curses.curs_set(v)
    except curses.error:
        pass  # terminal cannot hide/show the cursor


def main():
    try:
        dev = MPK()
        payload = dev.read(1)
    except (OSError, ValueError) as e:
        sys.exit(f"mpk3: {e}")
    os.environ.setdefault("ESCDELAY", "25")
    curses.wrapper(lambda scr: App(scr, dev, 1, payload).run())


if __name__ == "__main__":
    main()
