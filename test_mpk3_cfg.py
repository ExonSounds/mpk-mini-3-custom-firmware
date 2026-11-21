import os
import re
import unittest

from mpk3_cfg import (HDR, LAYOUT, PROG_BASE, PROG_RUN, PROG_SIZE, PROG_WRITE, SETTINGS, decode, encode, from_nibbles,
                      image_pixel, line_points, msg, parse_reply, program_messages, set_pixel, to_nibbles, to_oled)

FW = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fw")


def c_fields():
    """settings_t field names (arrays expanded to name1..nameN) from fw/settings.h, in order."""
    src = open(os.path.join(FW, "settings.h")).read()
    body = src[src.index("typedef struct {"):src.index("} settings_t;")]
    names = []
    for decl in re.findall(r"uint8_t\s+([^;]+);", body):
        for part in decl.split(","):
            m = re.match(r"\s*(\w+)(?:\[(\d+)\])?", part)
            names += [m[1]] if not m[2] else [f"{m[1]}{i}" for i in range(1, int(m[2]) + 1)]
    return names


class CfgTest(unittest.TestCase):
    def test_table_matches_firmware_struct_and_limits(self):
        self.assertEqual([s.key for s in SETTINGS], c_fields())
        src = open(os.path.join(FW, "settings.c")).read()
        lim = [int(x) for x in re.search(r"lim\[\] = \{([^}]*)\}", src)[1].split(",")]
        self.assertEqual([s.hi for s in SETTINGS[:len(lim)]], lim)  # single settings, checked one by one in C
        for s in SETTINGS[len(lim):]:                                # arrays: settings_valid() loops
            self.assertEqual(s.hi, 1 if s.key.startswith("knob_rel") else 2 if s.key == "idle_screen" else 127, s.key)
        self.assertEqual(SETTINGS[len(lim)].key, "knob_cc1")
        layout = int(re.search(r"#define SETTINGS_LAYOUT (\d+)", open(os.path.join(FW, "settings.h")).read())[1])
        self.assertEqual(layout, LAYOUT)

    def test_nibbles_roundtrip(self):
        raw = bytes(range(256))
        self.assertEqual(from_nibbles(to_nibbles(raw)), raw)
        self.assertTrue(all(b < 16 for b in to_nibbles(raw)))
        with self.assertRaises(ValueError):
            from_nibbles(b"\x01")

    def test_parse_reply_ignores_noise_and_realtime(self):
        reply = HDR + bytes([0x41, 7, 0, 3, 24]) + b"\xF7"
        noisy = b"\x90\x30\x40\xF8" + reply[:3] + b"\xF8" + reply[3:]
        self.assertEqual(parse_reply(noisy, 0x01), bytes([7, 0, 3, 24]))
        self.assertIsNone(parse_reply(reply[:-1], 0x01))
        self.assertIsNone(parse_reply(reply, 0x02))
        self.assertEqual(msg(2), HDR + b"\x02\xF7")

    def test_encode_decode_and_validation(self):
        raw = bytes(s.lo for s in SETTINGS)
        values = decode(raw)
        self.assertEqual(encode(values), raw)
        values["tempo"] = 241
        with self.assertRaisesRegex(ValueError, "Tempo"):
            encode(values)
        with self.assertRaises(ValueError):
            decode(raw[:-1])
        self.assertEqual(next(s for s in SETTINGS if s.key == "tempo").lo, 40)
        self.assertEqual(next(s for s in SETTINGS if s.key == "transpose").show(0), "-12")
        self.assertEqual(next(s for s in SETTINGS if s.key == "pad_note1").show(36), "36 (C1)")
        self.assertEqual(next(s for s in SETTINGS if s.key == "pad_note9").row, "Pad B1")

    def test_edit_text_roundtrip_and_errors(self):
        for s in SETTINGS:
            for v in (s.lo, s.hi):
                self.assertEqual(s.parse(s.text(v)), v)
        ch = SETTINGS[0]
        self.assertEqual(ch.parse(" 16 "), 15)
        for bad in ("0", "17", "x"):
            with self.assertRaisesRegex(ValueError, "channel"):
                ch.parse(bad)
        with self.assertRaisesRegex(ValueError, "choose one of"):
            next(s for s in SETTINGS if s.key == "arp_mode").parse("Sideways")


    def test_image_conversion(self):
        white, black = [[255] * 128 for _ in range(32)], [[0] * 128 for _ in range(32)]
        self.assertEqual(to_oled(white, 128, 32), b"\xff" * 512)
        self.assertEqual(to_oled(black, 128, 32), bytes(512))
        grey = to_oled([[128] * 128 for _ in range(32)], 128, 32)
        lit = sum(bin(b).count("1") for b in grey)
        self.assertTrue(4096 * 0.4 < lit < 4096 * 0.6)       # 50 % grey dithers to about half
        self.assertEqual(to_oled([[255] * 256 for _ in range(64)], 256, 64), b"\xff" * 512)  # same shape: fills
        wide = to_oled([[255] * 256 for _ in range(32)], 256, 32)  # twice as wide: 128x16, bars above and below
        self.assertTrue(image_pixel(wide, 64, 16) and not image_pixel(wide, 64, 2) and not image_pixel(wide, 64, 29))
        tall = to_oled([[255] * 32 for _ in range(32)], 32, 32)
        self.assertTrue(image_pixel(tall, 64, 16) and not image_pixel(tall, 10, 16) and not image_pixel(tall, 117, 16))
        one = bytearray(512)
        one[(9 >> 3) * 128 + 5] |= 1 << (9 & 7)              # pixel (5, 9) in the OLED's own layout
        self.assertTrue(image_pixel(bytes(one), 5, 9) and not image_pixel(bytes(one), 5, 8))

    def test_editor_helpers(self):
        img = bytearray(512)
        for x, y in ((0, 0), (127, 31), (5, 9)):
            set_pixel(img, x, y, True)
            self.assertTrue(image_pixel(img, x, y))
        set_pixel(img, 5, 9, False)
        self.assertFalse(image_pixel(img, 5, 9))
        self.assertTrue(image_pixel(img, 0, 0) and image_pixel(img, 127, 31))
        set_pixel(img, 128, 5, True)                       # off the edge: ignored
        set_pixel(img, -1, 5, True)
        self.assertEqual(sum(bin(b).count("1") for b in img), 2)
        for a, b in (((0, 0), (10, 3)), ((20, 30), (2, 1)), ((7, 7), (7, 7)), ((0, 31), (127, 0))):
            pts = line_points(*a, *b)
            self.assertEqual((pts[0], pts[-1]), (a, b))
            for p, q in zip(pts, pts[1:]):                 # every step goes to a neighbouring pixel: no gaps
                self.assertLessEqual(max(abs(p[0] - q[0]), abs(p[1] - q[1])), 1)

    def test_program_upload_messages(self):
        api = open(os.path.join(FW, "prog_api.h")).read()
        self.assertEqual(int(re.search(r"#define PROG_BASE (0x\w+)u", api)[1], 16), PROG_BASE)
        self.assertEqual(int(re.search(r"#define PROG_SIZE (0x\w+)u", api)[1], 16), PROG_SIZE)
        prog = b"MKRP" + bytes(range(256)) * 2 + b"\x01"           # 517 bytes: 9 blocks, last one padded
        msgs = program_messages(prog)
        self.assertEqual([c for c, _ in msgs], [PROG_WRITE] * 9 + [PROG_RUN])
        self.assertEqual(msgs[8][1][:2], bytes([0, 8]))
        self.assertEqual(b"".join(from_nibbles(d[2:]) for _, d in msgs[:-1])[:len(prog)], prog)
        run = msgs[-1][1]
        self.assertEqual(run[0] << 14 | run[1] << 7 | run[2], 517)
        self.assertEqual(run[3] << 14 | run[4] << 7 | run[5], sum(prog) & 0xFFFF)
        self.assertTrue(all(b < 128 for _, d in msgs for b in d))
        with self.assertRaisesRegex(ValueError, "header"):
            program_messages(b"\x7fELF" + bytes(60))
        with self.assertRaisesRegex(ValueError, "at most"):
            program_messages(b"MKRP" + bytes(PROG_SIZE))


if __name__ == "__main__":
    unittest.main()
