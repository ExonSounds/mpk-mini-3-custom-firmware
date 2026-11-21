import math
import os
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

from games import CATALOG
from games.beats import GOOD, LEAD_IN, Beats
from games.binterm import BinaryTerminal
from games.cave import Cave
from games.keycatch import ZONE, KeyCatch
from games.mouse import BTN_LEFT, BTN_RIGHT, TOP_SPEED, JoyMouse, NoMouse, VirtualMouse, speed
from games.race import CAR_X, CAR_Y, GRASS, LAPS, TARMAC, Race, build_world
from mpk3_cfg import PROG_MSG, image_pixel
from mpk3_market import Arcade
from mpk3_remote import Controls, Screen, idle_controls, pack7, parse_controls, pressed

WORLD = build_world()


def unpack7(p, n):  # mirror of unpack7() in programs/remote.c
    out = []
    for g in range(0, len(p), 8):
        out += [p[g + 1 + j] | (p[g] >> j & 1) << 7 for j in range(min(7, n - len(out)))]
    return bytes(out)


def ctrl(gas=0, rev=0, x=0):
    pads = [0] * 8
    pads[7], pads[3] = gas, rev
    return Controls(tuple(pads), x, 0, 0, (0,) * 25, (0,) * 8)


def drive(race, c, seconds, dt=1 / 30):
    for _ in range(int(seconds / dt)):
        race.step(c, dt)


class RemoteTest(unittest.TestCase):
    def test_pack7_roundtrip(self):
        data = bytes((i * 37 + 200) & 255 for i in range(128))
        p = pack7(data)
        self.assertEqual(len(p), 147)                     # fits one 149-byte FRAME message
        self.assertTrue(all(b < 128 for b in p))
        self.assertEqual(unpack7(p, 128), data)

    def test_parse_controls(self):
        r = bytes([1, 2, 3, 4, 5, 6, 7, 127]) + bytes([0, 0, 15, 80]) + bytes([0, 1]) + bytes(range(25)) + bytes(8)
        c = parse_controls(r)
        self.assertEqual((c.pads[7], c.joy_x, c.joy_y, c.buttons, c.keys[24]), (127, -1000, 1000, 1, 24))
        with self.assertRaises(ConnectionError):
            parse_controls(b"")

    def test_screen_text_uses_firmware_font(self):
        s = Screen()
        self.assertEqual(s.text(0, 0, "I"), 6)
        self.assertTrue(image_pixel(s.fb, 2, 0) and image_pixel(s.fb, 2, 6) and not image_pixel(s.fb, 0, 3))


class RaceTest(unittest.TestCase):
    def test_world(self):
        grid, path = WORLD
        self.assertGreater(len(path), 1000)
        r = Race(WORLD)
        self.assertEqual(r.cell(r.x, r.y), TARMAC)
        self.assertEqual(r.cell(5, 5), GRASS)

    def test_countdown_then_gas_moves_forward(self):
        r = Race(WORLD)
        drive(r, ctrl(gas=127), 2.9)
        self.assertEqual(r.v, 0)                          # no jump start
        drive(r, ctrl(gas=127), 1.2)
        self.assertEqual(r.state, "racing")
        self.assertGreater(r.v, 50)
        self.assertGreater(r.progress, -10)

    def test_steer_brake_reverse(self):
        r = Race(WORLD)
        drive(r, ctrl(), 3.1)
        h = r.heading
        drive(r, ctrl(x=1000), 0.5)
        self.assertEqual(r.heading, h)                    # standing still: steering does nothing
        drive(r, ctrl(gas=127, x=1000), 0.5)
        self.assertGreater(r.heading, h)                  # right = clockwise (y down)
        drive(r, ctrl(rev=127), 2.0)
        self.assertLess(r.v, 0)                           # braked, then reversing

    def test_laps_and_finish(self):
        r = Race(WORLD)
        drive(r, ctrl(), 3.1)
        n = len(r.path)
        for i in range(LAPS * n + 20):                    # move along the centre line, as if driving well
            r.x, r.y = r.path[(n - 10 + i) % n]
            r.v = 0
            r.step(ctrl(), 1 / 30)
        self.assertEqual(r.state, "finished")
        self.assertEqual(r.laps_done, LAPS)
        self.assertIsNotNone(r.best)

    def test_render(self):
        r, s = Race(WORLD), Screen()
        t = time.monotonic()
        r.render(s)
        self.assertLess(time.monotonic() - t, 0.05)       # fast enough for ~30 fps with the USB time
        self.assertTrue(image_pixel(s.fb, CAR_X, CAR_Y - 1))  # the car
        self.assertFalse(image_pixel(s.fb, CAR_X - 5, CAR_Y))  # its dark outline
        lit = sum(bin(b).count("1") for b in s.fb)
        self.assertTrue(100 < lit < 3000, lit)
        r.heading += math.pi / 2
        s2 = Screen()
        r.render(s2)
        self.assertNotEqual(s.fb, s2.fb)                  # the world turns with the car


def run(game, seconds, controls=None, dt=1 / 30):
    for _ in range(int(seconds / dt)):
        game.step(controls(game) if controls else idle_controls(), dt)


def pad8():
    return idle_controls(pads=(0,) * 7 + (100,))


class GamesTest(unittest.TestCase):
    def test_race_autopilot_finishes(self):                # the whole circuit can be driven
        r = Race(WORLD)
        run(r, 90, Race.autopilot)
        self.assertEqual(r.state, "finished")
        self.assertLess(r.best, 20)

    def test_pressed_edges(self):
        self.assertEqual(pressed((0, 5, 0, 9), (3, 5, 0, 0)), [0])

    def test_cave_autopilot_flies_and_crashing_ends(self):
        g = Cave(seed=1)
        run(g, 20, Cave.autopilot)
        self.assertEqual(g.state, "flying")
        self.assertGreater(g.dist, 500)
        run(g, 3, lambda g: idle_controls(knobs=(127,) + (64,) * 7))  # straight into the ceiling
        self.assertEqual(g.state, "crashed")
        self.assertGreater(g.best, 50)
        g.step(pad8(), 1 / 30)
        self.assertEqual(g.state, "flying")
        self.assertEqual(g.dist, 0)

    def test_keycatch_catch_and_miss(self):
        g = KeyCatch(seed=2)
        run(g, 1.2)
        k, y = g.notes[0]
        while g.notes[0][1] < ZONE[0] + 1:
            g.step(idle_controls(), 1 / 30)
        keys = [0] * 25
        keys[(k + 1) % 25] = 90
        g.step(idle_controls(keys=tuple(keys)), 1 / 30)  # wrong key: nothing
        self.assertEqual(g.score, 0)
        keys[k] = 90
        g.step(idle_controls(keys=tuple(keys)), 1 / 30)
        self.assertEqual(g.score, 1)
        run(g, 30)                                         # nobody plays: three misses
        self.assertEqual((g.state, g.lives, g.best), ("over", 0, 1))
        g.step(pad8(), 1 / 30)
        self.assertEqual((g.state, g.score, g.lives), ("playing", 0, 3))

    def test_beats_timing_and_end(self):
        g = Beats(seed=3)
        t, lane = g.chart[0]
        run(g, t - 0.01)
        pads = [0] * 8
        pads[lane] = 100
        g.step(idle_controls(pads=tuple(pads)), 0.01)
        self.assertEqual((g.hits, g.combo, g.word), (1, 1, "PERFECT"))
        run(g, 60)                                         # the rest goes by unplayed
        self.assertEqual(g.state, "done")
        self.assertEqual(g.combo, 0)
        self.assertLess(g.hits, g.total)
        g.step(pad8(), 1 / 30)
        self.assertEqual(g.state, "playing")
        self.assertGreater(len(g.chart), 20)
        self.assertGreaterEqual(g.chart[0][0], LEAD_IN - GOOD)

    def test_catalog_previews(self):
        self.assertEqual(len({e.key for e in CATALOG}), len(CATALOG))
        for e in CATALOG:
            s = Screen()
            e.preview(s)
            self.assertTrue(any(s.fb), e.key)
            if e.key in ("binterm", "mouse"):              # would open a terminal / make a mouse: tested below
                continue
            if e.kind == "computer":
                g, scr = e.make(), Screen()
                for _ in range(5):
                    g.step(idle_controls(), 1 / 30)
                g.render(scr)
            else:
                self.assertTrue(e.binary.endswith(".bin"))


class BinaryTyperTest(unittest.TestCase):
    def tap(self, g, *pads):
        for p in pads:
            down = [0] * 8
            down[p] = 90
            g.step(idle_controls(pads=tuple(down)), 1 / 30)
            g.step(idle_controls(), 1 / 30)

    def test_bits_become_letters(self):
        g = BinaryTerminal(launch=False)
        g.conn, theirs = socket.socketpair()
        g.state = "ready"
        self.tap(g, 0, 1, 0, 0, 0, 0, 0, 1)                    # 01000001
        self.tap(g, 0, 1, 1, 2, 0, 0, 1, 0, 0, 1)              # 011, delete a bit, then 001001: 01001001 = I
        self.tap(g, 2, 3)                                      # backspace (nothing pending), Enter
        self.tap(g, 1, 1, 1, 0, 1, 0, 0, 1)                    # 11101001 = é
        self.assertEqual(theirs.recv(64), b"AI\x7f\r" + "é".encode())
        self.assertEqual((g.bits, g.typed, g.shown), ("", "é", "é"))
        s = Screen()
        g.render(s)
        self.assertTrue(any(s.fb))
        theirs.close()                                         # the terminal window was closed
        g.step(idle_controls(), 1 / 30)
        self.assertEqual(g.state, "closed")
        g.close()

    def test_bridge_types_socket_input_into_the_shell(self):
        d = tempfile.mkdtemp()
        path = os.path.join(d, "s")
        server = socket.socket(socket.AF_UNIX)
        server.bind(path)
        server.listen(1)
        bridge = subprocess.Popen([sys.executable, "mpk3_termbridge.py", path, "cat"], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE)
        conn, _ = server.accept()
        conn.sendall(b"hi from pads\n")
        out, end = b"", time.monotonic() + 3
        while b"hi from pads" not in out and time.monotonic() < end:
            if select.select([bridge.stdout], [], [], 0.1)[0]:
                out += os.read(bridge.stdout.fileno(), 1024)
        self.assertIn(b"hi from pads", out)                    # cat (in the PTY) got it and echoed it
        bridge.kill()
        bridge.wait()
        bridge.stdin.close()
        bridge.stdout.close()
        conn.close()
        server.close()
        shutil.rmtree(d)


class JoyMouseTest(unittest.TestCase):
    def test_joystick_moves_and_pads_click(self):
        m = NoMouse()
        g = JoyMouse(mouse=m)
        g.step(idle_controls(joy_x=50, joy_y=-60), 0.1)        # inside the dead zone: still
        self.assertEqual(m.moves, [])
        for _ in range(30):                                    # half a second right and up at full tilt
            g.step(idle_controls(joy_x=1000, joy_y=1000), 1 / 60)
        dx, dy = sum(x for x, _ in m.moves), sum(y for _, y in m.moves)
        self.assertTrue(abs(dx - TOP_SPEED / 2) <= 2 and abs(dy + TOP_SPEED / 2) <= 2, (dx, dy))
        self.assertLess(speed(300), speed(1000) / 5)           # gentle near the middle for aiming
        pads = [0] * 8
        pads[7] = 60
        g.step(idle_controls(pads=tuple(pads)), 1 / 60)        # PAD 8 down, held, up
        g.step(idle_controls(pads=tuple(pads)), 1 / 60)
        g.step(idle_controls(), 1 / 60)
        pads = [0] * 8
        pads[3] = 60
        g.step(idle_controls(pads=tuple(pads)), 1 / 60)        # PAD 4
        g.step(idle_controls(), 1 / 60)
        self.assertEqual(m.buttons, [(BTN_LEFT, True), (BTN_LEFT, False), (BTN_RIGHT, True), (BTN_RIGHT, False)])
        s = Screen()
        g.render(s)
        self.assertTrue(any(s.fb))

    @unittest.skipUnless(os.access("/dev/uinput", os.W_OK), "no access to /dev/uinput")
    def test_virtual_mouse_appears_and_goes(self):             # makes the device but never moves or clicks
        vm = VirtualMouse()
        time.sleep(0.2)
        with open("/proc/bus/input/devices") as f:
            self.assertIn("MPK mini 3 joystick mouse", f.read())
        vm.close()
        time.sleep(0.2)
        with open("/proc/bus/input/devices") as f:
            self.assertNotIn("MPK mini 3 joystick mouse", f.read())


class FakeRemote:
    def __init__(self, fail_after=None):
        self.frames, self.closed, self.fail_after = 0, False, fail_after

    def show(self, fb):
        self.frames += 1
        if self.fail_after is not None and self.frames > self.fail_after:
            raise ConnectionError("remote screen stopped")
        return idle_controls()

    def close(self):
        self.closed = True


class FakeDevice:
    log = []

    def run_program(self, prog):
        FakeDevice.log.append(("run", prog[:4]))

    def request(self, cmd, data=b""):
        FakeDevice.log.append(("msg", cmd, data))
        return b"\x00"


class ArcadeTest(unittest.TestCase):
    def test_load_switch_unload(self):
        remotes, frames = [], []
        a = Arcade(device_factory=FakeDevice, remote_factory=lambda: remotes.append(FakeRemote()) or remotes[-1],
                   on_frame=frames.append)
        FakeDevice.log = []
        cave, race, fifths = (next(e for e in CATALOG if e.key == k) for k in ("cave", "race", "fifths"))
        a.load(cave)
        time.sleep(0.15)
        self.assertTrue(a.running() and frames and remotes[0].frames > 1)
        a.load(race)                                       # switching unloads the game before
        self.assertTrue(remotes[0].closed and not remotes[1].closed)
        self.assertEqual(a.current, race)
        a.load(fifths)                                     # a keyboard app: uploaded as a RAM program
        self.assertTrue(remotes[1].closed)
        self.assertEqual(FakeDevice.log, [("run", b"MKRP")])
        a.unload()
        self.assertEqual(FakeDevice.log[-1], ("msg", PROG_MSG, b"\x02"))
        self.assertIsNone(a.current)
        self.assertEqual(threading.active_count(), 1)

    def test_game_stopped_on_the_keyboard(self):
        a = Arcade(device_factory=FakeDevice, remote_factory=lambda: FakeRemote(fail_after=3))
        a.load(CATALOG[0])
        time.sleep(0.2)
        self.assertFalse(a.running())
        self.assertIn("stopped", a.stopped_by)
        a.unload()


if __name__ == "__main__":
    unittest.main()
