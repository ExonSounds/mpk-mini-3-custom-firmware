"""Binary Typer: opens a terminal window on this computer and types into it from the pads, one bit at a time."""
import os
import select
import shutil
import socket
import subprocess
import sys
import tempfile

from mpk3_remote import pressed

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BRIDGE = os.path.join(HERE, "mpk3_termbridge.py")
ZERO, ONE, UNDO, ENTER, REOPEN = 0, 1, 2, 3, 7  # PAD 1, 2, 3, 4, 8
# terminal emulators, first one installed wins: how each runs a command
TERMINALS = [("alacritty", ["-e"]), ("konsole", ["-e"]), ("gnome-terminal", ["--"]), ("kitty", []), ("foot", []),
             ("xterm", ["-e"])]
NAMES = {8: "BKSP", 9: "TAB", 10: "LF", 13: "ENTER", 27: "ESC", 32: "SPACE", 127: "DEL"}


def char_name(code):
    return NAMES.get(code) or (f"^{chr(code + 64)}" if code < 32 else chr(code))


class BinaryTerminal:
    TITLE = "Binary Typer"
    INFO = ("Opens a terminal on this computer and types into it from the pads in binary: eight bits make a "
            "letter, so 01000001 types A. Your shell keeps running when you unload it.")
    CONTROLS = "PAD 1: 0   PAD 2: 1   PAD 3: delete bit / backspace   PAD 4: Enter   PAD 8: reopen the terminal"

    def __init__(self, launch=True):
        self.bits, self.typed, self.shown, self.shown_t = "", "", "", 0.0
        self.pads, self.conn, self.server, self.proc, self.pending = (0,) * 8, None, None, None, b""
        self.dir = tempfile.mkdtemp(prefix="mpk3-term-")  # private (0700): only you can connect
        self.state = "opening"
        if launch:
            self._launch()

    def _launch(self):
        self.close_socket()
        path = os.path.join(self.dir, "typer.sock")
        if os.path.exists(path):
            os.unlink(path)
        self.server = socket.socket(socket.AF_UNIX)
        self.server.bind(path)
        self.server.listen(1)
        self.server.setblocking(False)
        term = next(((t, opt) for t, opt in TERMINALS if shutil.which(t)), None)
        if term is None:
            self.state = "no terminal"
            return
        self.proc = subprocess.Popen([term[0], *term[1], sys.executable, BRIDGE, path], start_new_session=True,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.state = "opening"

    def send(self, data):
        self.pending += data
        if self.conn:
            try:
                self.conn.sendall(self.pending)
                self.pending = b""
            except OSError:  # window closed
                self.conn, self.state = None, "closed"

    def step(self, ctrl, dt):
        tapped = pressed(self.pads, ctrl.pads)
        self.pads = ctrl.pads
        self.shown_t = max(0.0, self.shown_t - dt)
        if self.conn and select.select([self.conn], [], [], 0)[0] and not self.conn.recv(1):
            self.close_socket()                          # the bridge never writes: readable = window closed
            self.state = "closed"
        if self.conn is None and self.server is not None and self.state == "opening":
            try:
                self.conn, _ = self.server.accept()
                self.state = "ready"
                self.send(b"")
            except BlockingIOError:
                if self.proc and self.proc.poll() not in (None, 0):
                    self.state = "closed"  # the terminal could not start
        for pad in tapped:
            if pad == REOPEN and self.state in ("closed", "no terminal"):
                self._launch()
            elif pad in (ZERO, ONE):
                self.bits += "01"[pad]
                if len(self.bits) == 8:
                    self._type(int(self.bits, 2))
                    self.bits = ""
            elif pad == UNDO:
                if self.bits:
                    self.bits = self.bits[:-1]
                else:
                    self._type(127)
            elif pad == ENTER:
                self.bits = ""
                self._type(13)

    def _type(self, code):
        self.send(chr(code).encode())  # 128-255 as Latin-1 letters in UTF-8, e.g. 11101001 = é
        self.shown, self.shown_t = char_name(code), 1.0
        if code == 127:
            self.typed = self.typed[:-1]
        elif code == 13:
            self.typed = ""
        elif code >= 32:
            self.typed = (self.typed + chr(code))[-19:]

    def render(self, scr):
        scr.clear()
        for i in range(8):                               # the byte being typed, big; a gap after 4 bits
            x = i * 12 + (4 if i >= 4 else 0)
            if i < len(self.bits):
                scr.text(x, 0, self.bits[i], 2)
            else:
                scr.rect(x + 1, 12, 8, 2)
        if self.shown_t:                                 # the character just typed
            scr.text(102, 0, "=")
            if len(self.shown) == 1:
                scr.text(110, 0, self.shown, 2)
            else:
                scr.text(102, 8, self.shown[:4])
        status = {"opening": "OPENING TERMINAL...", "closed": "CLOSED. PAD 8: REOPEN",
                  "no terminal": "NO TERMINAL APP FOUND"}.get(self.state)
        scr.text(0, 16, status or "> " + self.typed.upper())
        scr.text(0, 25, "1=0 2=1 3=DEL 4=ENTER")

    def close_socket(self):
        for s in (self.conn, self.server):
            if s:
                s.close()
        self.conn = self.server = None

    def close(self):
        """Unloaded: stop typing; the terminal window and its shell stay open."""
        self.close_socket()
        shutil.rmtree(self.dir, ignore_errors=True)

    @classmethod
    def preview(cls, scr):
        g = cls(launch=False)
        g.state, g.typed, g.bits, g.shown, g.shown_t = "ready", "echo HI", "0100", "A", 1.0
        g.render(scr)
        g.close()
