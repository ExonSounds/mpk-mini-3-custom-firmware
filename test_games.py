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
from games.kwin import PointerTracker, capture, to_gray
from games.mouse import BTN_LEFT, BTN_RIGHT, PWM_FPS, TOP_SPEED, ZOOMS, JoyMouse, NoMouse, VirtualMouse, speed
from games.race import CAR_X, CAR_Y, GRASS, LAPS, TARMAC, Race, build_world
from games.sysmon import Stats, SysMon, human
from games.visualiser import BAL_RANGE, EDGES, N, RATE, Visualiser, note_name, stereo
import numpy as np
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
            if e.key in ("binterm", "mouse", "visualiser", "sysmon"):  # start helpers / devices: tested below
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

    def test_magnifier_follows_the_pointer(self):
        class Tracker:
            pos, closed = (1000, 500), False

            def poll(self):
                return self.pos

            def close(self):
                self.closed = True
        grabs = []

        def grab(x, y, w, h):                                  # left half white, right half black
            grabs.append((x, y, w, h))
            shot = np.zeros((h, w))
            shot[:, :w // 2] = 1.0
            return shot
        t = Tracker()
        g = JoyMouse(mouse=NoMouse(), tracker=t, grab=grab)
        pad = lambda i: idle_controls(pads=tuple(80 if j == i else 0 for j in range(8)))  # noqa: E731
        g.step(pad(0), 1 / 60)                                 # PAD 1: magnifier on (2x)
        self.assertEqual((g.magnify, g.note, grabs[-1]), (True, "MAGNIFIER 2X", (968, 492, 64, 16)))
        t.pos = (10, 20)                                       # the pointer moved (any mouse)
        g.step(idle_controls(), 1 / 60)
        self.assertEqual(grabs[-1], (-22, 12, 64, 16))
        g.step(pad(1), 1 / 60)                                 # PAD 2: 4x, then 1x
        g.step(idle_controls(), 1 / 60)
        self.assertEqual(grabs[-1], (-6, 16, 32, 8))
        g.step(pad(1), 1 / 60)
        self.assertEqual((g.note, grabs[-1]), ("ZOOM 1X", (-54, 4, 128, 32)))
        g.note_t = 0
        s = Screen()
        g.render(s)
        self.assertTrue(all(image_pixel(s.fb, x, 16) for x in range(64)))       # white half lit
        self.assertFalse(any(image_pixel(s.fb, x, 16) for x in range(64, 128)))
        g.step(pad(0), 1 / 60)                                 # back to the mouse view; the pointer still moves
        g.step(idle_controls(joy_x=1000), 0.1)
        self.assertFalse(g.magnify)
        self.assertTrue(g.mouse.moves)
        g.close()
        self.assertTrue(t.closed)

    def test_pwm_shades_by_flicker(self):
        class Tracker:
            def poll(self):
                return (500, 500)

            def close(self):
                pass

        def grab(x, y, w, h):                                  # four grey blocks: 0, 0.3, 0.6, 1.0
            return np.repeat(np.array([[0.0, 0.3, 0.6, 1.0]]), w // 4, axis=1).repeat(h, axis=0)
        g = JoyMouse(mouse=NoMouse(), tracker=Tracker(), grab=grab)
        pad = lambda i: idle_controls(pads=tuple(80 if j == i else 0 for j in range(8)))  # noqa: E731
        g.step(pad(0), 1 / 60)                                 # magnifier on
        g.step(idle_controls(), 1 / 60)
        self.assertEqual(g.fps, 60)                            # dither: the usual frame rate
        g.step(pad(2), 1 / 60)                                 # PAD 3: PWM
        self.assertEqual((g.note, g.fps), ("SHADES: PWM", PWM_FPS))
        g.note_t = 0
        lit = [0] * 4
        for _ in range(4):                                     # one full PWM cycle
            s = Screen()
            g.render(s)
            for block in range(4):
                lit[block] += image_pixel(s.fb, block * 32 + 16, 16)
        self.assertEqual(lit, [0, 1, 2, 4])                    # darker blocks are on for fewer frames
        g.step(pad(0), 1 / 60)                                 # leaving the magnifier: back to 60 fps
        self.assertEqual(g.fps, 60)

    def test_zoom_change_in_flicker_mode(self):            # was a crash: the old, smaller picture got reused
        class Tracker:
            def poll(self):
                return (500, 500)

            def close(self):
                pass
        g = JoyMouse(mouse=NoMouse(), tracker=Tracker(), grab=lambda x, y, w, h: np.random.rand(h, w))
        pad = lambda i: idle_controls(pads=tuple(80 if j == i else 0 for j in range(8)))  # noqa: E731
        g.step(pad(0), 1 / 60)
        for shade in range(2):                                 # PWM, then dither + PWM
            g.step(idle_controls(), 1 / 60)
            g.step(pad(2), 1 / 60)
            for _ in range(6):                                 # change the zoom at every point of the cycle
                g.render(Screen())
                g.step(idle_controls(), 1 / 60)
                g.step(pad(1), 1 / 60)
                self.assertEqual(g.shot.shape, (32 // ZOOMS[g.zoom], 128 // ZOOMS[g.zoom]))
                g.render(Screen())

    def test_dither_and_pwm_combined(self):
        greys = [0.1, 0.33, 0.53, 0.8]

        class Tracker:
            def poll(self):
                return (500, 500)

            def close(self):
                pass

        for v in greys:
            g = JoyMouse(mouse=NoMouse(), tracker=Tracker(), grab=lambda x, y, w, h, v=v: np.full((h, w), v))
            g.zoom = 0                                         # 1x: the whole screen is the grey area
            pad3 = idle_controls(pads=(0, 0, 80) + (0,) * 5)
            g.step(idle_controls(pads=(80,) + (0,) * 7), 1 / 60)  # magnifier on
            for _ in range(2):                                 # PAD 3 twice: PWM, then both
                g.step(idle_controls(), 1 / 60)
                g.step(pad3, 1 / 60)
            self.assertEqual((g.note, g.fps), ("SHADES: DITHER+PWM", PWM_FPS))
            g.shot = np.full((32, 128), v)                     # skip the contrast stretch: a flat area stays put
            g.shot[0, 0], g.shot[0, 1] = 0.0, 1.0              # (one black and one white pixel fix the range)
            g.note_t = 0
            frames = []
            for _ in range(4):
                s = Screen()
                g.render(s)
                frames.append([[image_pixel(s.fb, x, y) for x in range(128)] for y in range(1, 32)])
            f = np.array(frames, dtype=bool)
            self.assertAlmostEqual(f.mean(), v, delta=1 / 64 + 0.005, msg=v)  # 64 levels: close to the grey
            flicker = (f.any(0) & ~f.all(0)).mean()
            self.assertLess(flicker, 0.08, v)                  # only about 1 pixel in 16 flickers

    def test_kwin_pixels_to_brightness(self):
        rgbx = bytes([255, 0, 0, 0, 0, 0, 255, 0]) + bytes(8)  # 2 x 2 at stride 16: red, blue / padding
        meta = {"width": 2, "height": 1, "stride": 16, "format": 16}
        self.assertTrue(np.allclose(to_gray(rgbx, meta), [[0.299, 0.114]]))
        meta["format"] = 6                                     # ARGB32 premultiplied: bytes B, G, R, A
        self.assertTrue(np.allclose(to_gray(rgbx, meta), [[0.114, 0.299]]))

    @unittest.skipUnless(shutil.which("kwin_wayland") or os.environ.get("XDG_CURRENT_DESKTOP") == "KDE", "not KDE")
    def test_kwin_pointer_and_capture(self):                   # reads the real pointer and a tiny screen area
        t = PointerTracker()
        end = time.monotonic() + 2
        while not t.poll() and time.monotonic() < end:
            time.sleep(0.02)
        self.assertIsNotNone(t.pos)
        self.assertEqual(capture(t.pos[0], t.pos[1], 32, 8).shape, (8, 32))
        t.close()
        self.assertFalse(os.path.exists(t.dir))

    @unittest.skipUnless(os.access("/dev/uinput", os.W_OK), "no access to /dev/uinput")
    def test_virtual_mouse_appears_and_goes(self):             # makes the device but never moves or clicks
        def count():                                           # the market may have one of its own open
            with open("/proc/bus/input/devices") as f:
                return f.read().count("MPK mini 3 joystick mouse")
        before = count()
        vm = VirtualMouse()
        time.sleep(0.2)
        self.assertEqual(count(), before + 1)
        vm.close()
        time.sleep(0.2)
        self.assertEqual(count(), before)


class VisualiserTest(unittest.TestCase):
    def tone(self, freq, seconds=0.1, amp=0.5):
        t = np.arange(int(RATE * seconds)) / RATE
        return stereo(np.sin(2 * np.pi * freq * t) * amp)

    def test_spectrum_finds_the_tone(self):
        g = Visualiser(capture=False)
        data = self.tone(1000)
        g.feed(data[:1001])                                    # odd byte counts are carried over
        g.feed(data[1001:])
        g.step(idle_controls(), 1 / 60)
        loudest = int(np.argmax(g.level))
        self.assertTrue(EDGES[loudest] <= 1000 * 1.1 and EDGES[loudest + 1] >= 1000 / 1.1)
        self.assertLess(g.level[0], g.level[loudest] / 2)
        quieter = Visualiser(capture=False)
        quieter.feed(data)
        quieter.step(idle_controls(knobs=(20,) + (64,) * 7), 1 / 60)  # knob 1 down: less sensitive
        self.assertLess(quieter.level[loudest], g.level[loudest])

    def test_waveform_full_scale_touches_the_edges(self):
        g = Visualiser(capture=False)
        g.feed(self.tone(200, amp=1.0))
        g.step(idle_controls(pads=(0, 80) + (0,) * 6), 1 / 60)  # knob 1 centred: edges = 0 dBFS
        s = Screen()
        g.render(s)
        self.assertTrue(any(image_pixel(s.fb, x, 0) for x in range(128)))
        self.assertTrue(any(image_pixel(s.fb, x, 31) for x in range(128)))

        def rows(g, knob):
            g.step(idle_controls(knobs=(knob,) + (64,) * 7), 1 / 60)
            note = g.note
            g.note_t = 0                                       # no label over the picture
            s = Screen()
            g.render(s)
            lit = [y for y in range(32) if any(image_pixel(s.fb, x, y) for x in range(128))]
            return lit[0], lit[-1], note
        half = Visualiser(capture=False)                       # a -6 dB tone
        half.feed(self.tone(200, amp=0.5))
        half.step(idle_controls(pads=(0, 80) + (0,) * 6), 1 / 60)
        top, bottom, _ = rows(half, 64)
        self.assertTrue(6 <= top <= 9 and 22 <= bottom <= 25, (top, bottom))  # centred: half the height
        top, bottom, note = rows(half, 64 + 15)                # knob up 6 dB: zoomed to the edges
        self.assertEqual((top, bottom, note), (0, 31, "EDGE -6 DB"))
        top, bottom, _ = rows(half, 64 - 15)                   # knob down 6 dB: a quarter of the height
        self.assertTrue(10 <= top <= 13 and 18 <= bottom <= 21, (top, bottom))

    def test_pitch_follow_shows_two_cycles(self):
        pad4 = idle_controls(pads=(0, 0, 0, 80) + (0,) * 4)
        for freq in (110, 330, 880):
            g = Visualiser(capture=False)
            g.feed(self.tone(freq, amp=1.0))
            g.step(pad4, 1 / 60)                               # PAD 4: follow on (and the waveform view)
            self.assertEqual((g.follow, g.mode, g.note), (True, 1, "PITCH FOLLOW ON"))
            self.assertAlmostEqual(RATE / g.period, freq, delta=freq * 0.01)
            g.note_t = 0
            s = Screen()
            g.render(s)
            top = [image_pixel(s.fb, x, 0) for x in range(128)]
            peaks = sum(1 for x in range(1, 128) if top[x] and not top[x - 1])
            self.assertEqual(peaks, 2, freq)                   # exactly two cycles, whatever the pitch
        g.feed(stereo(np.random.default_rng(2).normal(0, 0.3, N)))
        g.step(idle_controls(), 1 / 60)                        # (PAD 4 let go)
        self.assertIsNone(g.period)                            # noise: no pitch, the normal 23 ms view
        g.render(Screen())
        g.step(pad4, 1 / 60)                                   # pressed again: off
        self.assertEqual((g.follow, g.note), (False, "PITCH FOLLOW OFF"))

    def test_note_names_match_the_keyboard(self):
        self.assertEqual(note_name(RATE / 261.63), "C3")       # middle C is C3 here, like the pad notes
        self.assertEqual(note_name(RATE / 440), "A3")

    def test_views_and_silence(self):
        g = Visualiser(capture=False)
        for p in range(3):
            pads = [0] * 8
            pads[p] = 80
            g.feed(self.tone(300))
            g.step(idle_controls(pads=tuple(pads)), 1 / 60)
            g.step(idle_controls(), 1 / 60)
            self.assertEqual(g.mode, p)
            s = Screen()
            g.render(s)
            self.assertTrue(any(s.fb), p)
        g.feed(bytes(4 * N))                                   # N stereo frames of silence
        for _ in range(150):                                   # 2.5 s of silence
            g.step(idle_controls(), 1 / 60)
        self.assertGreater(g.quiet, 2)
        s = Screen()
        g.render(s)
        self.assertTrue(any(s.fb))                             # the "play some audio" hint

    @unittest.skipUnless(shutil.which("parec") or shutil.which("pw-record"), "no audio recorder")
    def stereometer(self, left, right):
        g = Visualiser(capture=False)
        g.feed(stereo(left, right))
        g.step(idle_controls(pads=(0,) * 4 + (80,) + (0,) * 3), 1 / 60)  # PAD 5
        for _ in range(60):                                    # let the meters settle (1 s)
            g.step(idle_controls(), 1 / 60)
        def goniometer(v):                                    # its lit pixels below the L / R labels
            s = Screen()
            v.render(s)
            return {(x, y) for x in range(64) for y in range(8, 32) if image_pixel(s.fb, x, y)}
        guides = Visualiser(capture=False)
        guides.mode = 3
        return g, goniometer(g) - goniometer(guides)           # the plotted sound only

    def test_stereometer(self):
        t = np.arange(N) / RATE
        sine, rng = 0.8 * np.sin(2 * np.pi * 300 * t), np.random.default_rng(3)
        g, lit = self.stereometer(sine, sine)                  # mono: straight up the middle
        self.assertEqual(g.mode, 3)
        self.assertGreater(g.corr, 0.95)
        self.assertLess(abs(g.bal), 0.5)
        self.assertTrue(lit and all(abs(x - 32) <= 1 for x, y in lit), lit)
        self.assertTrue(any(y < 12 for x, y in lit))           # a loud mono sine reaches up high
        g, lit = self.stereometer(sine, -sine)                 # out of phase: flat along the bottom
        self.assertLess(g.corr, -0.95)
        self.assertTrue(any(x < 15 and y == 30 or y == 31 for x, y in lit))
        g, lit = self.stereometer(sine, 0 * sine)              # hard left: the left diagonal, balance left
        self.assertLess(g.bal, -BAL_RANGE)
        self.assertTrue(lit and all(x <= 32 and abs((31 - y) - (32 - x)) <= 1 for x, y in lit), lit)  # left 45°
        g, _ = self.stereometer(rng.normal(0, 0.2, N), rng.normal(0, 0.2, N))  # unrelated noise: wide, no corr.
        self.assertLess(abs(g.corr), 0.15)
        s = Screen()
        g.render(s)
        self.assertTrue(any(s.fb))

    def test_records_from_this_computer(self):
        g = Visualiser()
        time.sleep(0.5)
        g.step(idle_controls(), 1 / 60)
        proc = g.proc
        g.close()
        self.assertIsNotNone(proc.returncode)                  # the recorder is gone again


class SysMonTest(unittest.TestCase):
    def fake_system(self, idle, net_rx):
        """A tiny /proc and /sys: 2 CPU threads, one sensor chip of each kind, eth0 plus lo."""
        root = self.root
        files = {
            "proc/stat": f"cpu  100 0 100 {idle} 0 0 0 0 0 0\ncpu0 50 0 50 {idle // 2} 0 0 0 0\n"
                         f"cpu1 50 0 50 {idle // 2} 0 0 0 0\nintr 1 2 3\n",
            "proc/meminfo": "MemTotal:       16000000 kB\nMemFree:  1 kB\nMemAvailable:    8000000 kB\n",
            "proc/net/dev": "Inter-|\n face |\n    lo: 999999 0 0 0 0 0 0 0 999999 0\n"
                            f"  eth0: {net_rx} 0 0 0 0 0 0 0 5000 0\n",
            "proc/uptime": "93784.5 1000.0\n", "proc/loadavg": "1.25 0.9 0.5 1/100 42\n",
            "sys/class/hwmon/hwmon0/name": "k10temp\n", "sys/class/hwmon/hwmon0/temp1_input": "45500\n",
            "sys/class/hwmon/hwmon0/temp1_label": "Tctl\n",
            "sys/class/hwmon/hwmon1/name": "nvme\n", "sys/class/hwmon/hwmon1/temp1_input": "30000\n",
            "sys/class/hwmon/hwmon2/name": "nct6798\n", "sys/class/hwmon/hwmon2/temp1_input": "31000\n",
            "sys/class/hwmon/hwmon2/temp1_label": "SYSTIN\n", "sys/class/hwmon/hwmon2/fan1_input": "1200\n",
            "sys/class/hwmon/hwmon2/fan2_input": "0\n",
            "sys/class/drm/card0/device/gpu_busy_percent": "40\n",
        }
        for rel, text in files.items():
            os.makedirs(os.path.dirname(os.path.join(root, rel)), exist_ok=True)
            with open(os.path.join(root, rel), "w") as f:
                f.write(text)

    def test_stats_from_proc_and_sys(self):
        self.root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.root)
        self.fake_system(idle=1000, net_rx=10000)
        s = Stats(root=self.root, gpu=False)
        s.sample(0)
        self.fake_system(idle=1100, net_rx=10000 + 2 * 1024 * 1024)  # 100 more idle, 100 more busy jiffies
        with open(os.path.join(self.root, "proc/stat")) as f:
            stat = f.read().replace("cpu  100 0 100", "cpu  150 0 150")  # total busy +100, idle +100
        with open(os.path.join(self.root, "proc/stat"), "w") as f:
            f.write(stat)
        s.sample(2.0)
        self.assertAlmostEqual(s.cpu, 50.0)
        self.assertEqual(len(s.cores), 2)
        self.assertEqual(s.mem, (8000000 * 1024, 16000000 * 1024))
        self.assertEqual(s.net, (1024 * 1024, 0.0))                     # lo ignored; 2 MiB in 2 s
        self.assertEqual(s.temps, {"CPU": 45.5, "SSD": 30.0, "BRD": 31.0, "GPU": 0.0})
        self.assertEqual(s.fans, [1200])                                # stopped fans left out
        self.assertEqual(s.gpu[0], 40.0)                                # AMD busy percent
        self.assertEqual(s.uptime(), ("1D 2:03", "1.25"))
        self.assertEqual((human(812), human(1300), human(35 * 2 ** 20), human(1.5 * 2 ** 30)),
                         ("812B", "1.3K", "35M", "1.5G"))

    def test_pages_and_sampling(self):
        self.root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.root)
        self.fake_system(idle=1000, net_rx=0)
        g = SysMon(stats=Stats(root=self.root, gpu=False))
        for page in range(4):
            pads = [0] * 8
            pads[page] = 80
            g.step(idle_controls(pads=tuple(pads)), 0.01)
            g.step(idle_controls(), 0.01)
            self.assertEqual(g.page, page)
            s = Screen()
            g.render(s)
            self.assertTrue(any(s.fb), page)
        before = len([h for h in g.history if h != (0.0, 0.0)])
        for _ in range(20):                                             # 1 s: two more readings
            g.step(idle_controls(), 0.05)
        self.assertEqual(len([h for h in g.history if h != (0.0, 0.0)]) - before, 2)
        g.close()


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
