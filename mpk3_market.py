#!/usr/bin/env python3
"""MPK Game Market: pick a game and it is loaded onto the keyboard, replacing the one that was there.

Games that run on this computer use the keyboard as screen and controller (programs/remote.bin); keyboard apps are
RAM programs that run on the keyboard itself. Needs firmware v9.80+ and the programs built (make -C programs).
usage: python3 mpk3_market.py
"""
import threading
import time
import tkinter as tk
from tkinter import messagebox, ttk

from games import CATALOG
from mpk3_cfg import IMG_H, IMG_W, PROG_MSG, Device, image_pixel
from mpk3_remote import QUIT, Remote, Screen, play

CARD_SCALE, MIRROR_SCALE = 2, 4
BG, CARD, INK, DIM, ACCENT = "#15171c", "#22252d", "#e8e8ea", "#9aa0aa", "#ffb347"


class Arcade:
    """Keeps one catalog entry loaded on the keyboard; load() unloads the one before."""

    def __init__(self, device_factory=Device, remote_factory=Remote, on_frame=None):
        self.device_factory, self.remote_factory, self.on_frame = device_factory, remote_factory, on_frame
        self.current, self.stopped_by = None, None   # stopped_by: why a computer game ended by itself
        self._thread = self._stop = self._remote = self._game = None

    def load(self, entry):
        self.unload()
        if entry.kind == "computer":
            self._remote = self.remote_factory()     # starts the remote screen program on the keyboard
            self._stop, self.stopped_by, self._game = threading.Event(), None, entry.make()
            self._thread = threading.Thread(target=self._run, args=(self._game,), daemon=True)
            self._thread.start()
        else:
            with open(entry.binary, "rb") as f:
                prog = f.read()
            dev = self.device_factory()
            for attempt in range(25):                # the program before may still be on its way out
                try:
                    dev.run_program(prog)
                    break
                except IOError as e:
                    if "busy" not in str(e) or attempt == 24:
                        raise IOError(f"{e} - hold PROG SELECT for 2 s on the keyboard to stop it") from e
                    time.sleep(0.04)
        self.current = entry

    def _run(self, game):
        try:
            play(game, self._remote, self._stop, self.on_frame)
        except (ConnectionError, OSError, TimeoutError) as e:
            self.stopped_by = str(e)
        except Exception as e:  # a bug in a game: say so in the market instead of only in the terminal
            self.stopped_by = f"the game hit an error ({type(e).__name__}: {e})"

    def running(self):
        """False once a computer game ended by itself (PROG SELECT on the keyboard, unplugged)."""
        return self.current is not None and (self.current.kind == "keyboard" or self._thread.is_alive())

    def unload(self):
        """Stop what is loaded; the keyboard goes back to its normal screen."""
        entry, self.current = self.current, None
        if entry is None:
            return
        if entry.kind == "computer":
            self._stop.set()
            self._thread.join(3)
            self._remote.close()                     # QUIT: the remote screen program returns
            if hasattr(self._game, "close"):         # apps that hold something on this computer (Binary Typer)
                self._game.close()
            self._thread = self._stop = self._remote = self._game = None
        else:
            self.device_factory().request(PROG_MSG, bytes([QUIT]))  # empty answer: it already left


def draw_fb(canvas, fb, scale, cells=None):
    """Draw a 128x32 frame; with cells (from make_cells) only recolours, which is fast enough for live video."""
    if cells is None:
        canvas.delete("all")
        for y in range(IMG_H):
            for x in range(IMG_W):
                if image_pixel(fb, x, y):
                    canvas.create_rectangle(x * scale, y * scale, (x + 1) * scale - 1, (y + 1) * scale - 1,
                                            fill=INK, width=0)
        return
    old = cells["fb"]
    for i in range(512):
        if fb[i] != old[i]:
            x, page, diff = i % 128, i // 128, fb[i] ^ old[i]
            for bit in range(8):
                if diff >> bit & 1:
                    canvas.itemconfig(cells["ids"][page * 8 + bit][x], fill=INK if fb[i] >> bit & 1 else "black")
    cells["fb"] = bytes(fb)


def make_cells(canvas, scale):
    ids = [[canvas.create_rectangle(x * scale, y * scale, (x + 1) * scale - 1, (y + 1) * scale - 1, fill="black",
                                    width=0) for x in range(IMG_W)] for y in range(IMG_H)]
    return {"ids": ids, "fb": bytes(512)}


class Market:
    def __init__(self, root, arcade=None):
        self.root, self.frame = root, None
        self.arcade = arcade or Arcade()
        self.arcade.on_frame = self._frame
        root.title("MPK Game Market")
        root.configure(bg=BG)
        style = ttk.Style(root)
        style.theme_use("clam")
        style.configure("Load.TButton", background=ACCENT, foreground="black", padding=(14, 4))
        style.map("Load.TButton", background=[("active", "#ffc777"), ("disabled", "#555")])
        style.configure("Stop.TButton", padding=(14, 4))

        head = tk.Frame(root, bg=BG)
        head.pack(fill="x", padx=16, pady=(14, 4))
        tk.Label(head, text="MPK Game Market", font=("TkDefaultFont", 20, "bold"), fg=INK, bg=BG).pack(anchor="w")
        tk.Label(head, text="Pick a game and it is loaded onto the keyboard, replacing the one before.",
                 fg=DIM, bg=BG).pack(anchor="w")

        grid = tk.Frame(root, bg=BG)
        grid.pack(padx=10, pady=6)
        self.buttons = {}
        for i, entry in enumerate(CATALOG):
            self._card(grid, entry).grid(row=i // 5, column=i % 5, padx=6, pady=6, sticky="nsew")

        now = tk.Frame(root, bg=CARD, padx=12, pady=10)
        now.pack(fill="x", padx=16, pady=(4, 8))
        top = tk.Frame(now, bg=CARD)
        top.pack(fill="x")
        self.now_title = tk.StringVar()
        tk.Label(top, text="ON THE KEYBOARD", fg=ACCENT, bg=CARD, font=("TkDefaultFont", 9, "bold")).pack(side="left")
        tk.Label(top, textvariable=self.now_title, fg=INK, bg=CARD).pack(side="left", padx=10)
        self.unload_btn = ttk.Button(top, text="Unload", style="Stop.TButton", command=self.unload)
        self.unload_btn.pack(side="right")
        self.mirror = tk.Canvas(now, width=IMG_W * MIRROR_SCALE, height=IMG_H * MIRROR_SCALE, bg="black",
                                highlightthickness=0)
        self.mirror.pack(pady=(8, 0))
        self.cells = make_cells(self.mirror, MIRROR_SCALE)
        self.status = tk.StringVar(value="Ready")
        tk.Label(root, textvariable=self.status, fg=DIM, bg=BG, anchor="w").pack(fill="x", padx=16, pady=(0, 8))
        root.protocol("WM_DELETE_WINDOW", self.quit)
        self._refresh()
        self._tick()

    def _card(self, parent, entry):
        card = tk.Frame(parent, bg=CARD, padx=10, pady=10)
        pic = tk.Canvas(card, width=IMG_W * CARD_SCALE, height=IMG_H * CARD_SCALE, bg="black", highlightthickness=0)
        pic.pack()
        scr = Screen()
        entry.preview(scr)
        draw_fb(pic, scr.fb, CARD_SCALE)
        tk.Label(card, text=entry.title, fg=INK, bg=CARD, font=("TkDefaultFont", 13, "bold")).pack(anchor="w",
                                                                                                    pady=(8, 0))
        where = "Runs on this computer, plays on the keyboard" if entry.kind == "computer" else "Runs on the keyboard itself"
        tk.Label(card, text=where, fg=ACCENT, bg=CARD, font=("TkDefaultFont", 8)).pack(anchor="w")
        tk.Label(card, text=entry.info, fg=INK, bg=CARD, wraplength=IMG_W * CARD_SCALE, justify="left").pack(
            anchor="w", pady=(4, 0))
        tk.Label(card, text=entry.controls, fg=DIM, bg=CARD, wraplength=IMG_W * CARD_SCALE, justify="left",
                 font=("TkDefaultFont", 8)).pack(anchor="w", pady=(4, 8))
        b = ttk.Button(card, text="Load", style="Load.TButton", command=lambda: self.load(entry))
        b.pack(anchor="e", side="bottom")
        self.buttons[entry.key] = b
        return card

    def _frame(self, fb):  # from the game thread: keep the newest frame for the mirror
        self.frame = fb

    def _act(self, fn, ok):
        self.root.config(cursor="watch")
        self.root.update_idletasks()
        try:
            fn()
            self.status.set(ok)
        except Exception as e:  # device errors carry readable messages
            self.status.set(f"Error: {e}")
            messagebox.showerror("MPK Game Market", str(e))
        finally:
            self.root.config(cursor="")
            self._refresh()

    def load(self, entry):
        self._act(lambda: self.arcade.load(entry), f"{entry.title} loaded - play it on the keyboard")
        scr = Screen()
        if self.arcade.current is entry and entry.kind == "keyboard":
            entry.preview(scr)                           # its own screen can't be mirrored; show what it looks like
        draw_fb(self.mirror, scr.fb, MIRROR_SCALE, self.cells)

    def unload(self):
        self._act(self.arcade.unload, "Unloaded - the keyboard is back on its normal screen")
        draw_fb(self.mirror, bytes(512), MIRROR_SCALE, self.cells)

    def _refresh(self):
        cur = self.arcade.current
        for key, b in self.buttons.items():
            b.config(text="Playing" if cur and cur.key == key else "Load")
        self.unload_btn.state(["!disabled"] if cur else ["disabled"])
        if cur is None:
            self.now_title.set("Nothing loaded - the keyboard shows its normal screen")
        elif cur.kind == "computer":
            self.now_title.set(f"{cur.title} - live picture of the keyboard's screen")
        else:
            self.now_title.set(f"{cur.title} - running on the keyboard (PROG SELECT there leaves it)")

    def _tick(self):
        cur = self.arcade.current
        if self.frame is not None and cur is not None and cur.kind == "computer":
            draw_fb(self.mirror, self.frame, MIRROR_SCALE, self.cells)
            self.frame = None
        if cur is not None and not self.arcade.running():
            why = self.arcade.stopped_by or "stopped"
            self._act(self.arcade.unload, f"{cur.title} stopped: {why}")
            draw_fb(self.mirror, bytes(512), MIRROR_SCALE, self.cells)
        self.root.after(80, self._tick)

    def quit(self):
        if self.arcade.current is not None:
            try:
                self.arcade.unload()
            except Exception:  # closing anyway; PROG SELECT on the keyboard resets it
                pass
        self.root.destroy()


def main():
    root = tk.Tk()
    Market(root)
    root.mainloop()


if __name__ == "__main__":
    main()
