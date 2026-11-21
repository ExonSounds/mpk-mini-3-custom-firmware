import os
import re
import unittest

from mpk3_cfg import HDR, LAYOUT, SETTINGS, decode, encode, from_nibbles, msg, parse_reply, to_nibbles

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
            self.assertEqual(s.hi, 1 if s.key.startswith("knob_rel") or s.key == "idle_screen" else 127, s.key)
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


if __name__ == "__main__":
    unittest.main()
