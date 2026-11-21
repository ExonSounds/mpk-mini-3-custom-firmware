"""USB tests: the emulator (mpk3emu) runs the real firmware images and the repo's tools talk to it over the MIDI pty
and the ALSA sequencer port, like a plugged-in keyboard.

usage: python3 -m unittest emu/test_usb.py   (needs emu/mpk3emu and fw/mpk.bin; stock tests need
       firmware/stock_v1.26/AT.bin; ALSA tests skip without a sequencer)
"""
import ctypes
import ctypes.util
import os
import re
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
import mpk3  # noqa: E402
import mpk3_cfg  # noqa: E402
import mpk3_flash  # noqa: E402

EMU = os.path.join(HERE, "mpk3emu")
CUSTOM = os.path.join(ROOT, "fw", "mpk.bin")
STOCK = os.path.join(ROOT, "firmware", "stock_v1.26", "AT.bin")


class Emu:
    """One emulator process with its own flash file and run dir."""

    def __init__(self, tmp, image=None, fast=True):
        self.dir, self.flash = os.path.join(tmp, "run"), os.path.join(tmp, "flash")
        self.log = open(os.path.join(tmp, "log"), "w+")
        args = [EMU, self.flash, "--run-dir", self.dir] + (["--image", image] if image else []) + (["--fast"] if fast else [])
        self.p, self.panel = subprocess.Popen(args, stdout=self.log, stderr=self.log), None
        self.midi = os.path.join(self.dir, "midi")
        end = time.monotonic() + 15
        while not (os.path.exists(os.path.join(self.dir, "name")) and os.path.exists(self.midi)):
            if time.monotonic() > end or self.p.poll() is not None:
                self.stop()
                raise AssertionError("emulator did not enumerate:\n" + self.text())
            time.sleep(0.05)
        self.panel = socket.socket(socket.AF_UNIX)
        self.panel.connect(os.path.join(self.dir, "panel.sock"))
        self.lines = self.panel.makefile("r")

    def text(self):
        self.log.flush()
        self.log.seek(0)
        return self.log.read()

    def name(self):
        with open(os.path.join(self.dir, "name")) as f:
            return f.read().strip()

    def seq_addr(self):
        m = re.search(r"ALSA seq (\d+):(\d+)", self.text())
        return (int(m[1]), int(m[2])) if m and m[1] != "-1" else None

    def send(self, line):
        self.panel.sendall((line + "\n").encode())

    def power_cycle(self, timeout=15):
        """Power cycle (held inputs stay held) and wait until the host has enumerated the device again."""
        self.send("power")
        self.panel.settimeout(timeout)
        gone = False  # the server replays the last status on connect: skip an "enumerated" from before the cycle
        for line in self.lines:
            gone = gone or line.startswith("status USB disconnected")
            if gone and line.startswith("status USB enumerated"):
                return line.split(": ", 1)[1].strip()

    def stop(self):
        if self.panel:
            self.lines.close()
            self.panel.close()
        if self.p.poll() is None:
            self.p.terminate()
            self.p.wait(10)
        self.log.close()


def read_for(fd, seconds):
    buf, end = b"", time.monotonic() + seconds
    while time.monotonic() < end:
        if select.select([fd], [], [], 0.02)[0]:
            buf += os.read(fd, 4096)
    return buf


class Event(ctypes.Structure):  # snd_seq_event_t (28 bytes)
    _pack_ = 1
    _fields_ = [("type", ctypes.c_uint8), ("flags", ctypes.c_uint8), ("tag", ctypes.c_uint8), ("queue", ctypes.c_uint8),
                ("time", ctypes.c_uint32 * 2), ("src", ctypes.c_uint8 * 2), ("dest", ctypes.c_uint8 * 2),
                ("len", ctypes.c_uint32), ("ptr", ctypes.c_void_p)]  # data.ext; data.note is the first 3 bytes


class Seq:
    """A minimal ALSA sequencer client (ctypes on libasound) connected both ways to the emulator's port."""
    NOTEON, SYSEX = 6, 130

    def __init__(self, addr):
        self.lib = ctypes.CDLL(ctypes.util.find_library("asound") or "libasound.so.2")
        self.h = ctypes.c_void_p()
        if self.lib.snd_seq_open(ctypes.byref(self.h), b"default", 3, 1) < 0:  # duplex, nonblocking
            raise unittest.SkipTest("no ALSA sequencer")
        self.lib.snd_seq_set_client_name(self.h, b"mpk3emu test")
        self.port = self.lib.snd_seq_create_simple_port(self.h, b"test", 0x63, 2 | 1 << 20)  # R W SUBS_R SUBS_W
        assert self.lib.snd_seq_connect_from(self.h, self.port, *addr) == 0
        assert self.lib.snd_seq_connect_to(self.h, self.port, *addr) == 0

    def events(self, seconds):
        out, end, ev = [], time.monotonic() + seconds, ctypes.POINTER(Event)()
        while time.monotonic() < end:
            if self.lib.snd_seq_event_input(self.h, ctypes.byref(ev)) < 0:
                time.sleep(0.01)
                continue
            e = ev.contents
            data = ctypes.string_at(e.ptr, e.len) if e.type == self.SYSEX else ctypes.string_at(ctypes.addressof(e) + 16, 3)
            out.append((e.type, data))
        return out

    def sysex(self, data):
        buf = ctypes.create_string_buffer(data, len(data))
        e = Event(type=self.SYSEX, flags=4, queue=253, src=(ctypes.c_uint8 * 2)(0, self.port),
                  dest=(ctypes.c_uint8 * 2)(254, 253), len=len(data), ptr=ctypes.cast(buf, ctypes.c_void_p))
        assert self.lib.snd_seq_event_output_direct(self.h, ctypes.byref(e)) >= 0

    def close(self):
        self.lib.snd_seq_close(self.h)


@unittest.skipUnless(os.path.exists(EMU) and os.path.exists(CUSTOM), "needs emu/mpk3emu and fw/mpk.bin")
class CustomTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(dir=os.environ.get("XDG_RUNTIME_DIR"))
        self.emu = Emu(self.tmp, CUSTOM)
        self.dev = mpk3_cfg.Device(self.emu.midi)

    def tearDown(self):
        self.emu.stop()
        shutil.rmtree(self.tmp)

    def test_enumerates_and_publishes(self):
        self.assertEqual(self.emu.name(), "MPK mini 3 custom")
        self.assertTrue(os.readlink(self.emu.midi).startswith("/dev/pts/"))
        self.emu.stop()
        self.assertFalse(os.path.exists(self.emu.midi) or os.path.exists(os.path.join(self.emu.dir, "name")))

    def test_settings_save_survives_restart(self):
        self.assertEqual(self.dev.hello()["size"], len(mpk3_cfg.SETTINGS))
        v = self.dev.read()
        v["tempo"] = 97 if v["tempo"] != 97 else 98
        self.dev.write(v)
        self.assertEqual(self.dev.read()["tempo"], v["tempo"])
        self.dev.save()
        self.emu.stop()
        self.emu = Emu(self.tmp)  # same flash file, no --image
        self.assertEqual(mpk3_cfg.Device(self.emu.midi).read()["tempo"], v["tempo"])

    def test_key_notes_on_pty_and_alsa(self):
        addr = self.emu.seq_addr()
        if not addr:
            self.skipTest("no ALSA sequencer")
        seq = Seq(addr)
        try:
            fd = os.open(self.emu.midi, os.O_RDWR | os.O_NONBLOCK)
            read_for(fd, 0.2)
            self.emu.send("key 0 100")
            time.sleep(0.3)
            self.emu.send("key 0 0")
            got = read_for(fd, 0.5)
            self.assertIn(bytes([0x90, 0x30]), got)
            self.assertIn(bytes([0x80, 0x30]), got)
            self.assertIn((Seq.NOTEON, bytes([0, 0x30, got[got.index(0x90) + 2]])), seq.events(0.3))
            # computer -> device over ALSA: a SysEx HELLO gets the firmware's reply on both endpoints
            seq.sysex(mpk3_cfg.msg(mpk3_cfg.HELLO))
            evs = seq.events(1.0)
            self.assertTrue(any(t == Seq.SYSEX and d.startswith(mpk3_cfg.HDR + bytes([0x41])) for t, d in evs), evs)
            self.assertIsNotNone(mpk3_cfg.parse_reply(read_for(fd, 0.2), mpk3_cfg.HELLO))
            os.close(fd)
        finally:
            seq.close()

    def test_external_clock(self):
        v = self.dev.read()
        v["clock_ext"] = 1
        self.dev.write(v)
        fd = os.open(self.emu.midi, os.O_RDWR | os.O_NONBLOCK)
        self.emu.send("button 4 1")  # ARP on
        time.sleep(0.1)
        self.emu.send("button 4 0")
        self.emu.send("key 0 100")
        self.assertLessEqual(read_for(fd, 0.5).count(0x90), 1)  # no clock: the arp waits
        for _ in range(48):  # two quarter notes of clock = 8 sixteenths
            os.write(fd, b"\xf8")
            time.sleep(0.01)
        self.assertGreaterEqual(read_for(fd, 0.3).count(0x90), 6)
        os.close(fd)

    def test_running_status_and_long_sysex(self):
        fd = os.open(self.emu.midi, os.O_RDWR | os.O_NONBLOCK)
        # notes in running status, a clock byte inside the SysEx, then a GET (reply > 1 USB packet)
        os.write(fd, bytes([0x90, 60, 1, 62, 1, 0x80, 60, 0, 62, 0]))
        req = mpk3_cfg.msg(mpk3_cfg.GET)
        os.write(fd, req[:3] + b"\xf8" + req[3:])
        buf, end = b"", time.monotonic() + 2
        while mpk3_cfg.parse_reply(buf, mpk3_cfg.GET) is None and time.monotonic() < end:
            buf += read_for(fd, 0.05)
        self.assertEqual(len(mpk3_cfg.from_nibbles(mpk3_cfg.parse_reply(buf, mpk3_cfg.GET))), len(mpk3_cfg.SETTINGS))
        os.close(fd)


@unittest.skipUnless(os.path.exists(EMU) and os.path.exists(STOCK), "needs emu/mpk3emu and firmware/stock_v1.26/AT.bin")
class StockTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(dir=os.environ.get("XDG_RUNTIME_DIR"))
        self.emu = Emu(self.tmp, STOCK)

    def tearDown(self):
        self.emu.stop()
        shutil.rmtree(self.tmp)

    def test_enumerates_and_reads_programs(self):
        self.assertEqual(self.emu.name(), "MPK mini 3")
        self.assertIn("(09e8:1049)", self.emu.text())
        m = mpk3.MPK(self.emu.midi)
        for prog in (0, 1, 8):
            p = m.read(prog)
            self.assertEqual((p[0], bytes(p[1:5])), (prog, b"PGM:"))

    def test_write_program(self):
        m = mpk3.MPK(self.emu.midi)
        p = m.read(2)
        p[28] = 100 if p[28] != 100 else 101  # tempo
        m.write(2, p)  # verifies by reading back

    def hid_reply(self, cmd):
        fd = os.open(os.path.join(self.emu.dir, "hid"), os.O_RDWR)
        try:
            os.write(fd, b"\x00" + mpk3_flash.cmd(cmd))
            self.assertTrue(select.select([fd], [], [], 3)[0], "no HID reply")
            return os.read(fd, 64)
        finally:
            os.close(fd)

    @unittest.skipUnless(os.path.exists(CUSTOM), "needs fw/mpk.bin")
    def test_update_mode_flashes_custom_firmware(self):
        self.assertEqual(self.hid_reply(mpk3_flash.CMD_CONNECT)[:3], b"UI\xe4")  # the running app refuses
        self.emu.send("button 3 1")  # hold PAD BANK + PROG SELECT over a power cycle: update mode
        self.emu.send("button 0 1")
        self.emu.power_cycle()
        fd = os.open(os.path.join(self.emu.dir, "hid"), os.O_RDWR)
        try:
            mpk3_flash.flash(fd, open(CUSTOM, "rb").read(), progress=lambda s: None)
        finally:
            os.close(fd)
        self.emu.send("button 3 0")
        self.emu.send("button 0 0")
        self.assertEqual(self.emu.power_cycle(), "MPK mini 3 custom")
        self.assertEqual(mpk3_cfg.Device(self.emu.midi).hello()["layout"], mpk3_cfg.LAYOUT)


if __name__ == "__main__":
    unittest.main()
