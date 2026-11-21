"""Tests of the panel socket client and the front panel GUI against a fake emulator.

usage: python3 -m unittest emu/test_panel.py     (GUI tests skip when there is no display)
"""
import os
import shutil
import socket
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu_client import Panel, pixel  # noqa: E402

FB = bytes((i * 7) & 0xFF for i in range(512))
OLED = "oled 1 200 0 " + FB.hex()
LEDS = "leds " + bytes(range(0, 240, 10)).hex()


class FakeEmu:
    """Speaks panel_srv.c's side: greets with hello + the latest oled/leds, records commands, answers sync."""

    def __init__(self, path, greet=(OLED, LEDS), answer_sync=True):
        self.path, self.greet, self.answer_sync = path, greet, answer_sync
        self.lines, self.conns, self.cv = [], [], threading.Condition()
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(path)
        self.srv.listen()
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self):
        while True:
            try:
                conn, _ = self.srv.accept()
            except OSError:
                return
            self.conns.append(conn)
            conn.sendall("".join(line + "\n" for line in ("hello mpk3emu 1", *self.greet)).encode())
            threading.Thread(target=self._read, args=(conn,), daemon=True).start()

    def _read(self, conn):
        for line in conn.makefile("r"):
            line = line.rstrip("\n")
            with self.cv:
                self.lines.append(line)
                self.cv.notify_all()
            if line.startswith("sync ") and self.answer_sync:
                conn.sendall((line + "\n").encode())
            if line.startswith("peek "):  # fake memory: byte i of address a = (a + i) & 0xff, unmapped above 1 MB
                a, n = map(int, line.split()[1:])
                data = bytes((a + i) & 0xFF for i in range(n)) if a < 1 << 20 else b""
                conn.sendall(f"peek {a} {data.hex()}\n".encode())

    def emit(self, line):
        for c in self.conns:
            c.sendall((line + "\n").encode())

    def wait_for(self, line, timeout=3):
        with self.cv:
            return self.cv.wait_for(lambda: line in self.lines, timeout)

    def take(self):
        with self.cv:
            out, self.lines[:] = list(self.lines), []
            return out

    def stop(self):
        self.srv.close()
        for c in self.conns:
            try:
                c.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            c.close()
        os.unlink(self.path)


class ClientTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.path = os.path.join(self.dir, "panel.sock")
        self.emu = FakeEmu(self.path)

    def tearDown(self):
        self.emu.stop()
        shutil.rmtree(self.dir)

    def test_frames_leds_status(self):
        got = []
        p = Panel(self.path, on_oled=lambda *a: got.append(a), on_status=got.append)
        self.assertTrue(p.wait(lambda p: p.oled and p.leds))
        self.assertEqual(p.hello, "mpk3emu 1")
        self.assertEqual(p.oled, (True, 200, False, FB))
        self.assertEqual(p.leds, bytes(range(0, 240, 10)))
        fb = bytearray(512)
        fb[128 + 5] = 0x04  # page 1, x 5, bit 2 -> y 10
        self.emu.emit("oled 0 17 1 " + fb.hex())
        self.emu.emit("status USB enumerated")
        self.assertTrue(p.wait(lambda p: p.statuses))
        self.assertEqual(p.oled, (False, 17, True, bytes(fb)))
        self.assertEqual(pixel(p.oled[3], 5, 10), 1)
        self.assertEqual(pixel(p.oled[3], 5, 9), 0)
        self.assertEqual(got[-1], "USB enumerated")
        self.assertEqual(p.frames, 2)
        p.close()

    def test_wire_format(self):
        p = Panel(self.path)
        p.key(24, 100), p.key(0, 0), p.button(3, True), p.button(3, 0), p.pad(7, 127), p.pad(0, 0)
        p.knob(2, -15), p.knob(1, 7.6), p.joy(-1000, 500), p.pedal(1), p.pedal(False), p.power()
        p.sync()
        p.quit()
        self.assertTrue(self.emu.wait_for("quit"))
        self.assertEqual(self.emu.take(), ["key 24 100", "key 0 0", "button 3 1", "button 3 0", "pad 7 127",
                                           "pad 0 0", "knob 2 -15", "knob 1 8", "joy -1000 500", "pedal 1",
                                           "pedal 0", "power", "sync 1", "quit"])
        self.assertEqual(p.peek(0x1000, 4), bytes([0, 1, 2, 3]))
        self.assertIn("peek 4096 4", self.emu.take())
        with self.assertRaisesRegex(IOError, "could not read"):
            p.peek(1 << 24, 4)
        p.sync()  # numbers count up, so a late answer can't satisfy a later sync
        self.assertIn("sync 2", self.emu.take())
        p.close()

    def test_sync_timeout_and_close(self):
        self.emu.answer_sync = False
        closed = threading.Event()
        p = Panel(self.path, on_close=closed.set)
        with self.assertRaises(TimeoutError):
            p.sync(timeout=0.2)
        self.emu.stop()
        self.assertTrue(closed.wait(2))
        self.assertFalse(p.connected)
        with self.assertRaises(ConnectionError):
            p.key(0, 1)
        self.emu = FakeEmu(self.path)  # for tearDown

    def test_not_running(self):
        with self.assertRaisesRegex(ConnectionError, "not running"):
            Panel(os.path.join(self.dir, "nothing.sock"))


@unittest.skipUnless(os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"), "no display")
class PanelGuiTest(unittest.TestCase):
    def setUp(self):
        import tkinter as tk
        import panel
        self.panel = panel
        self.dir = tempfile.mkdtemp()
        self.path = os.path.join(self.dir, "panel.sock")
        self.emu = FakeEmu(self.path)
        self.root = tk.Tk()  # stays mapped: event_generate needs a visible window
        self.app = panel.FrontPanel(self.root, self.path)
        self.assertTrue(self.until(lambda: self.app.shown_leds))
        self.emu.take()

    def tearDown(self):
        self.root.destroy()
        self.emu.stop()
        shutil.rmtree(self.dir)

    def until(self, pred, timeout=3):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.root.update()
            if pred():
                return True
            time.sleep(0.01)
        return False

    def expect(self, *lines):
        self.assertTrue(self.until(lambda: len(self.emu.lines) >= len(lines), 2), self.emu.lines)
        self.assertEqual(self.emu.take(), list(lines))

    def at(self, kind, i, fy=0.5):  # canvas point inside a control, fy = fraction of its height
        x0, y0, x1, y1 = self.app.geom[kind, i]
        return dict(x=int((x0 + x1) / 2), y=int(y0 + (y1 - y0) * fy))

    def click(self, kind, i, fy=0.5, button=1, **kw):
        self.app.c.event_generate(f"<ButtonPress-{button}>", **self.at(kind, i, fy), **kw)

    def test_oled_and_leds(self):
        img = self.app.oled_img
        on = img.get(5 * 4, 0)  # FB byte 5 = 0x23: bit 0 lit; byte 4 = 0x1c: bit 0 dark
        self.assertGreater(on[2], 150)
        self.assertEqual(img.get(4 * 4, 0), (0, 0, 0))
        self.assertEqual(img.get(5 * 4 + 3, 0), (0, 0, 0))  # gap between dots
        self.emu.emit("oled 1 255 1 " + FB.hex())  # inverted
        self.assertTrue(self.until(lambda: self.app.oled_img is not img))
        self.assertEqual(self.app.oled_img.get(5 * 4, 0), (0, 0, 0))
        self.assertNotEqual(self.app.oled_img.get(4 * 4, 0), (0, 0, 0))
        self.emu.emit("oled 0 255 0 " + bytes([255] * 512).hex())  # display off: black
        self.assertTrue(self.until(lambda: self.app.shown_oled[0] is False))
        self.assertEqual(self.app.oled_img.get(0, 0), (0, 0, 0))
        pad = self.app.items["pad", 0]
        self.emu.emit("leds " + bytes([0] * 8 + [255] + [0] * 15).hex())
        self.assertTrue(self.until(lambda: self.app.c.itemcget(pad, "fill") == "#ff3c32"))
        self.emu.emit("status reset")
        self.assertTrue(self.until(lambda: "reset" in self.app.status.get()))

    def test_keys(self):
        self.click("key", 0, 1.0)  # bottom edge = hardest
        self.app.c.event_generate("<ButtonRelease-1>", x=0, y=0)
        self.click("key", 1, 0.0, state=1)  # black key, Shift = 127
        self.app.c.event_generate("<ButtonRelease-1>", x=0, y=0)
        self.click("key", 4, 0.0)
        self.app.c.event_generate("<ButtonRelease-1>", x=0, y=0)
        self.expect("key 0 127", "key 0 0", "key 1 127", "key 1 0", "key 4 1", "key 4 0")
        self.app.velocity.set(90)
        self.app.c.event_generate("<KeyPress>", keysym="z")
        self.app.c.event_generate("<KeyPress>", keysym="i")
        self.app.c.event_generate("<KeyRelease>", keysym="z")
        self.app.c.event_generate("<KeyPress>", keysym="z")  # autorepeat: no new note
        self.app.c.event_generate("<KeyRelease>", keysym="z")
        self.app.c.event_generate("<KeyRelease>", keysym="i")
        self.expect("key 0 90", "key 24 90", "key 0 0", "key 24 0")
        self.click("key", 12, button=3)  # latch: stays down until right-clicked again
        self.root.update()
        self.click("key", 12, button=3)
        self.expect("key 12 64", "key 12 0")

    def test_pads_knobs_joystick(self):
        self.click("pad", 4, 0.5)
        y = self.at("pad", 4)["y"]
        self.app.c.event_generate("<B1-Motion>", x=820, y=y - 10, state=0x100)  # drag up = more pressure
        self.app.c.event_generate("<Button-5>", x=820, y=y)                    # wheel down while held
        self.app.c.event_generate("<ButtonRelease-1>", x=820, y=y)
        self.expect("pad 4 64", "pad 4 74", "pad 4 66", "pad 4 0")
        self.app.c.event_generate("<Button-4>", **self.at("knob", 2))
        self.app.c.event_generate("<Button-5>", **self.at("knob", 2))
        self.click("knob", 7)
        p = self.at("knob", 7)
        self.app.c.event_generate("<B1-Motion>", x=p["x"], y=p["y"] - 5, state=0x100)
        self.app.c.event_generate("<ButtonRelease-1>", x=0, y=0)
        self.expect("knob 2 10", "knob 2 -10", "knob 7 10")
        x, y, r = self.panel.JOY
        self.app.c.event_generate("<ButtonPress-1>", x=x + r, y=y)
        self.app.c.event_generate("<B1-Motion>", x=x + 500, y=y - r // 2, state=0x100)
        self.app.c.event_generate("<ButtonRelease-1>", x=x + 500, y=y - r // 2)
        self.expect("joy 1000 0", "joy 1000 493", "joy 0 0")

    def test_buttons_pedal_power(self):
        self.click("button", 5)
        self.app.c.event_generate("<ButtonRelease-1>", x=0, y=0)
        self.click("button", 3, button=3)  # latch PAD BANK + PROG SELECT, power cycle, let go of both
        self.click("button", 0, button=3)
        self.click("power", 0)
        self.app.c.event_generate("<ButtonRelease-1>", x=0, y=0)
        self.click("button", 3, button=3)
        self.click("button", 0, button=3)
        self.click("pedal", 0)
        self.click("pedal", 0)
        self.expect("button 5 1", "button 5 0", "button 3 1", "button 0 1", "power", "button 3 0", "button 0 0",
                    "pedal 1", "pedal 0")

    def test_reconnect(self):
        self.click("button", 9, button=3)
        self.expect("button 9 1")
        self.emu.stop()
        self.assertTrue(self.until(lambda: "stopped" in self.app.status.get()))
        self.assertFalse(self.app.holders.get(("button", 9)))
        self.assertTrue(self.until(lambda: "not running" in self.app.status.get()))
        self.emu = FakeEmu(self.path)
        self.assertTrue(self.until(lambda: "Connected" in self.app.status.get()))
        self.expect("pedal 0")


if __name__ == "__main__":
    unittest.main()
