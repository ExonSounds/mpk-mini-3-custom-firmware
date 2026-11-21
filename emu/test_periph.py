"""Phase-2 peripheral models against both firmwares, driven through the panel socket (emu/panel_srv.c).

    make -C emu check        (or: cd emu && python3 -m unittest test_periph -v)

Needs fw/mpk.bin + fw/mpk.elf (make -C fw); the stock tests also need the owner's local
firmware/stock_v1.26/AT.bin and are skipped without it. Flash files and sockets go to
$XDG_RUNTIME_DIR/mpk3-emu-periph-test (never into the repo).
"""
import os
import shutil
import socket
import subprocess
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EMU = os.path.join(HERE, "mpk3emu")
CUSTOM = os.path.join(ROOT, "fw", "mpk.bin")
CUSTOM_ELF = os.path.join(ROOT, "fw", "mpk.elf")
STOCK = os.path.join(ROOT, "firmware", "stock_v1.26", "AT.bin")
RUN = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or tempfile.gettempdir(), "mpk3-emu-periph-test")


def pixel(fb, x, y):
    return fb[(y >> 3) * 128 + x] >> (y & 7) & 1


class Emu:
    """One emulator process (real-time pacing, so sleeps here are emulated time too) and its panel socket."""

    def __init__(self, name, image):
        os.makedirs(RUN, exist_ok=True)
        self.dir = os.path.join(RUN, name)
        shutil.rmtree(self.dir, ignore_errors=True)
        flash = os.path.join(RUN, name + ".flash")
        if os.path.exists(flash):
            os.unlink(flash)
        self.log = open(os.path.join(RUN, name + ".log"), "w")
        self.proc = subprocess.Popen([EMU, flash, "--image", image, "--run-dir", self.dir], stderr=self.log)
        self.sock = socket.socket(socket.AF_UNIX)
        for _ in range(100):
            try:
                self.sock.connect(os.path.join(self.dir, "panel.sock"))
                break
            except OSError:
                time.sleep(0.05)
        else:
            raise TimeoutError("emulator socket did not appear")
        self.sock.settimeout(0.05)
        self.buf, self.oled, self.leds, self.replies, self.seq = b"", None, None, [], 0

    def close(self):
        self.sock.close()
        self.proc.terminate()
        self.proc.wait(5)
        self.log.close()

    def send(self, *lines):
        self.sock.sendall("".join(l + "\n" for l in lines).encode())

    def pump(self, seconds=0.0):
        """Read panel output for this long (wall time = emulated time)."""
        end = time.time() + seconds
        while True:
            try:
                self.buf += self.sock.recv(1 << 16)
            except socket.timeout:
                pass
            *lines, self.buf = self.buf.split(b"\n")
            for l in lines:
                w = l.decode().split()
                if w and w[0] == "oled":
                    self.oled = (int(w[1]), int(w[2]), int(w[3]), bytes.fromhex(w[4]))
                elif w and w[0] == "leds":
                    self.leds = list(bytes.fromhex(w[1]))
                elif w and w[0] in ("peek", "sync"):
                    self.replies.append(w)
            if time.time() >= end:
                return

    def wait(self, cond, timeout=5.0):
        end = time.time() + timeout
        while not cond():
            if time.time() > end:
                raise TimeoutError("condition not met")
            self.pump(0.05)

    def peek(self, addr, n):
        self.replies = []
        self.send("peek %d %d" % (addr, n))
        self.wait(lambda: any(r[0] == "peek" for r in self.replies))
        return bytes.fromhex(next(r for r in self.replies if r[0] == "peek")[2])

    def u16(self, addr):
        return int.from_bytes(self.peek(addr, 2), "little")


def host_home_frame(**state):
    """fb of fw/views.c draw_home() built for this computer (like fw/test_keys.c)."""
    d = tempfile.mkdtemp()
    src = os.path.join(d, "home.c")
    with open(src, "w") as f:
        f.write('#include <stdio.h>\n#include "views.h"\n#include "board.h"\nint main(void) { home_t h = {%s};'
                ' draw_home(&h); for (int i = 0; i < 512; i++) printf("%%02x", fb[i]); return 0; }\n'
                % ", ".join(".%s = %d" % kv for kv in state.items()))
    exe = os.path.join(d, "home")
    fw = os.path.join(ROOT, "fw")
    subprocess.check_call(["cc", "-std=c11", "-I", fw, "-o", exe, src, os.path.join(fw, "views.c"), os.path.join(fw, "gfx.c")])
    out = bytes.fromhex(subprocess.check_output([exe]).decode())
    shutil.rmtree(d)
    return out


def elf_symbol(name):
    for line in subprocess.check_output(["llvm-nm", CUSTOM_ELF]).decode().splitlines():
        w = line.split()
        if len(w) == 3 and w[2] == name:
            return int(w[0], 16)
    raise KeyError(name)


@unittest.skipUnless(os.path.exists(CUSTOM) and os.path.exists(CUSTOM_ELF), "make -C fw first")
class Custom(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.e = Emu("custom", CUSTOM)
        cls.e.wait(lambda: cls.e.oled and cls.e.oled[0] and any(cls.e.oled[3]))
        cls.e.pump(0.5)

    @classmethod
    def tearDownClass(cls):
        cls.e.close()

    def test_home_screen_pixels(self):
        # defaults (fw/settings.c): tempo 130, internal clock; the USB square depends on the USB model
        fb = self.e.oled[3]
        want = [host_home_frame(octave=0, bpm=130, usb=u) for u in (0, 1)]
        self.assertIn(fb, want)
        self.assertEqual(self.e.oled[:3], (1, 0xCF, 0))

    def test_velocity(self):
        kvel, kstate = elf_symbol("kvel"), elf_symbol("kstate")  # fw/main.c: struck velocity, 0 idle / 2 on
        for v in (1, 10, 30, 64, 90, 115, 127):
            self.e.send("key 0 %d" % v)
            self.e.wait(lambda: self.e.peek(kstate, 1)[0] == 2)
            got = self.e.peek(kvel, 1)[0]
            self.e.send("key 0 0")
            self.e.wait(lambda: self.e.peek(kstate, 1)[0] == 0)
            self.assertLessEqual(abs(got - v), 3, "velocity %d read back as %d" % (v, got))

    def test_leds_follow_buttons(self):
        self.e.wait(lambda: self.e.leds)
        self.assertGreater(self.e.leds[0], 240)       # PAD BANK A lit
        self.e.send("button 6 1")                      # OCT+
        self.e.pump(0.1)
        self.e.send("button 6 0")
        self.e.wait(lambda: 20 < self.e.leds[18] < 45)  # OCT+ LED at 4/32
        self.e.send("button 7 1")
        self.e.pump(0.1)
        self.e.send("button 7 0")
        self.e.wait(lambda: self.e.leds[18] == 0)

    def test_pad_knob_joystick(self):
        self.e.send("pad 0 100")
        self.e.wait(lambda: self.e.leds[8] > 240)     # pad 1 LED
        self.e.send("pad 0 0")
        self.e.wait(lambda: self.e.leds[8] == 0)
        kn = elf_symbol("kn")  # analog.c knob state: {u16 a, b; i16 acc; u8 val, out, init} = 10 bytes, val at +6
        self.e.send("knob 2 90")
        self.e.wait(lambda: self.e.peek(kn + 2 * 10 + 6, 1)[0] > 30)  # clockwise = up
        self.e.send("knob 2 -45")
        self.e.wait(lambda: self.e.peek(kn + 2 * 10 + 6, 1)[0] < 30)
        joy = elf_symbol("joy")  # analog.c: {u16 c, lo, hi; i16 lastr, ...} = 18 bytes per axis, lastr at +6
        lastr = lambda ax: int.from_bytes(self.e.peek(joy + 18 * ax + 6, 2), "little", signed=True)
        self.e.send("joy 1000 -1000")  # the custom fw scales by the range it has seen, so go to the edges
        self.e.wait(lambda: lastr(0) == 1000 and lastr(1) == -1000)
        self.e.send("joy 0 0")
        self.e.wait(lambda: lastr(0) == 0 and lastr(1) == 0)


@unittest.skipUnless(os.path.exists(STOCK), "no local stock image (firmware/get_stock_firmware.sh)")
class Stock(unittest.TestCase):
    TABLE = 0x20003318  # analog readings, 8 bytes per entry: channel * 8 + mux position, 24 = PA3, 25 = PA4

    @classmethod
    def setUpClass(cls):
        cls.e = Emu("stock", STOCK)
        cls.e.wait(lambda: cls.e.oled and cls.e.oled[0] and sum(bin(b).count("1") for b in cls.e.oled[3]) > 100,
                   timeout=10)
        cls.e.pump(0.5)

    @classmethod
    def tearDownClass(cls):
        cls.e.close()

    def test_screen(self):
        fb = self.e.oled[3]
        os.makedirs(RUN, exist_ok=True)
        with open(os.path.join(RUN, "stock.pgm"), "wb") as f:  # for a look: P5 128x32
            f.write(b"P5 128 32 1\n" + bytes(pixel(fb, x, y) for y in range(32) for x in range(128)))
        self.assertTrue(any(pixel(fb, x, y) for x in range(128) for y in range(8)))     # "PGM:..." line
        self.assertTrue(any(pixel(fb, x, y) for x in range(64, 128) for y in range(16, 24)))  # "BPM:..."

    def entry(self, i):
        return self.e.u16(self.TABLE + 8 * i)

    def test_analog_table(self):
        # joystick entries: value scaled by stock's auto-calibration (0-4095), then raw, min, max
        self.assertAlmostEqual(self.entry(24), 2048, delta=200)  # X centre
        self.e.send("joy 1000 -1000")
        self.e.wait(lambda: self.entry(24) > 4000 and self.entry(25) < 100)
        self.e.send("joy 0 0", "pad 0 127")                       # pad 1 = PA2, mux position 3
        self.e.wait(lambda: self.entry(2 * 8 + 3) > 3700)
        self.e.send("pad 0 0")
        self.e.wait(lambda: self.entry(2 * 8 + 3) < 100)
        a = self.entry(0 * 8 + 0)                                 # knob 2 wiper A (mux position 0)
        self.e.send("knob 1 45")
        self.e.wait(lambda: abs(self.entry(0) - a) > 500)

    def test_leds_follow_buttons_and_pads(self):
        self.e.wait(lambda: self.e.leds)
        self.e.send("pad 0 100")
        self.e.wait(lambda: self.e.leds[8] > 100)                 # pad 1 LED
        self.e.send("pad 0 0")
        self.e.wait(lambda: self.e.leds[8] == 0)
        bank = self.e.leds[0] > 128
        self.e.send("button 3 1")                                 # PAD BANK: A <-> B (LEDs 0/1)
        self.e.pump(0.1)
        self.e.send("button 3 0")
        self.e.wait(lambda: (self.e.leds[1] > 128) == bank and (self.e.leds[0] > 128) != bank)


if __name__ == "__main__":
    unittest.main()
