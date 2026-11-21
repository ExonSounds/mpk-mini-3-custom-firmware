"""Client for the emulator's front-panel socket (protocol: the header comment of emu/panel_srv.c).

    from emu_client import Panel, pixel
    p = Panel()                    # connects to $XDG_RUNTIME_DIR/mpk3-emu/panel.sock (or Panel(path))
    p.key(0, 100); p.sync()        # press the lowest C; sync() returns once the emulator applied it
    p.wait(lambda p: p.oled)       # wait for the first picture
    on, contrast, invert, fb = p.oled; pixel(fb, x, y)   # fb: 512 bytes, SSD1306 page order
    p.leds                         # 24 brightnesses 0-255 (index table in firmware/NOTES.md)
    p.peek(0x20000000, 16)         # debug/tests: bytes of emulated memory
    p.close()

The reader thread keeps the newest picture/LEDs; callbacks (on_oled(on, contrast, invert, fb), on_leds(levels),
on_status(text), on_close()) run on that thread, so GUIs should only store what they get and draw from their own loop.
No dependencies beyond the standard library.
"""
import itertools
import os
import socket
import threading


def default_socket():
    return os.path.join(os.environ.get("XDG_RUNTIME_DIR") or "/tmp", "mpk3-emu", "panel.sock")


def pixel(fb, x, y):
    """1 if pixel (x, y) of a 128x32 frame is lit (before invert)."""
    return fb[(y >> 3) * 128 + x] >> (y & 7) & 1


class Panel:
    def __init__(self, path=None, timeout=2.0, on_oled=None, on_leds=None, on_status=None, on_close=None):
        self.path = path or default_socket()
        self.on_oled, self.on_leds, self.on_status, self.on_close = on_oled, on_leds, on_status, on_close
        self.hello = self.oled = self.leds = None   # oled = (on, contrast, invert, fb bytes)
        self.statuses, self.frames, self.connected = [], 0, False
        self._synced, self._peeks, self._seq = set(), {}, itertools.count(1)
        self._cv, self._send_lock = threading.Condition(), threading.Lock()
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            self.sock.settimeout(timeout)
            self.sock.connect(self.path)
            self.sock.settimeout(None)
        except OSError as e:
            self.sock.close()
            raise ConnectionError(f"emulator not running? cannot connect to {self.path} ({e.strerror or e})") from e
        self.connected = True
        threading.Thread(target=self._read, daemon=True).start()

    # ---- inputs (wire format: one line per command, see panel_srv.c) ----
    def key(self, k, velocity):    self._send(f"key {int(k)} {int(velocity)}")      # velocity 0 = release
    def button(self, b, down):     self._send(f"button {int(b)} {int(bool(down))}")
    def pad(self, p, force):       self._send(f"pad {int(p)} {int(force)}")         # force 0 = released
    def knob(self, n, degrees):    self._send(f"knob {int(n)} {int(round(degrees))}")
    def joy(self, x, y):           self._send(f"joy {int(x)} {int(y)}")             # -1000..1000, + = right / up
    def pedal(self, down):         self._send(f"pedal {int(bool(down))}")
    def power(self):               self._send("power")
    def quit(self):                self._send("quit")

    def sync(self, timeout=5.0):
        """Return once the emulator applied every command sent before; TimeoutError if it doesn't answer."""
        n = next(self._seq)
        self._send(f"sync {n}")
        if not self.wait(lambda p: n in p._synced or not p.connected, timeout) or n not in self._synced:
            raise TimeoutError(f"emulator did not answer sync {n} within {timeout} s")
        self._synced.discard(n)

    def peek(self, addr, n, timeout=5.0):
        """n (1-512) bytes of emulated memory at addr (below 0x80000000: the server parses a signed int)."""
        with self._cv:
            self._peeks.pop(addr, None)
        self._send(f"peek {int(addr)} {int(n)}")
        if not self.wait(lambda p: addr in p._peeks or not p.connected, timeout) or addr not in self._peeks:
            raise TimeoutError(f"emulator did not answer peek {addr:#x} within {timeout} s")
        data = self._peeks.pop(addr)
        if len(data) != n:
            raise IOError(f"emulator could not read {n} bytes at {addr:#x}")
        return data

    def wait(self, pred, timeout=5.0):
        """Wait until pred(self) is true (checked after every message); returns its last value."""
        with self._cv:
            return self._cv.wait_for(lambda: pred(self), timeout)

    def close(self):
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()

    def _send(self, line):
        with self._send_lock:
            try:
                self.sock.sendall(line.encode() + b"\n")
            except OSError as e:
                raise ConnectionError(f"emulator connection lost ({e.strerror or e})") from e

    def _read(self):
        buf = b""
        try:
            while True:
                data = self.sock.recv(65536)
                if not data:
                    break
                buf += data
                *lines, buf = buf.split(b"\n")
                for line in lines:
                    self._handle(line.decode(errors="replace"))
        except OSError:
            pass
        with self._cv:
            self.connected = False
            self._cv.notify_all()
        if self.on_close:
            self.on_close()

    def _handle(self, line):
        verb, _, rest = line.partition(" ")
        cb = None
        with self._cv:
            try:
                if verb == "oled":
                    on, contrast, invert, hexdata = rest.split()
                    self.oled = (on == "1", int(contrast), invert == "1", bytes.fromhex(hexdata))
                    self.frames += 1
                    cb = self.on_oled and (lambda: self.on_oled(*self.oled))
                elif verb == "leds":
                    self.leds = bytes.fromhex(rest)
                    cb = self.on_leds and (lambda: self.on_leds(self.leds))
                elif verb == "status":
                    self.statuses.append(rest)
                    del self.statuses[:-100]
                    cb = self.on_status and (lambda: self.on_status(rest))
                elif verb == "sync":
                    self._synced.add(int(rest))
                elif verb == "peek":
                    addr, _, data = rest.partition(" ")
                    self._peeks[int(addr)] = bytes.fromhex(data)
                elif verb == "hello":
                    self.hello = rest
            except ValueError:  # malformed line: ignore it rather than kill the reader
                return
            self._cv.notify_all()
        if cb:
            cb()
