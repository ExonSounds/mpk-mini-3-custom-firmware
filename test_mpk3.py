import os
import tempfile
import unittest
from unittest import mock

import mpk3_cfg
from mpk3 import FIELDS, PAYLOAD_LEN, find_device, parse_reply, write_msg

# Real reply captured from an MPK mini 3 for program 1 ("Live").
REPLY = bytes.fromhex(
    "f0 47 00 49 67 01 76 01 4c 69 76 65 00 00 00 00 00 00 00 00 00 00 00 00 09 01 00 04 00 00 04 01"
    " 00 00 03 01 02 00 00 00 00 02 01 01 24 00 10 25 01 11 26 02 12 27 03 13 28 04 14 29 05 15 2a 06"
    " 16 2b 07 17 2c 08 18 2d 09 19 2e 0a 1a 2f 0b 1b 30 0c 1c 31 0d 1d 32 0e 1e 33 0f 1f 00 01 00 7f"
    " 4b 6e 6f 62 20 31 2d 31 00 00 00 00 00 00 00 00 00 02 00 7f 4b 6e 6f 62 20 31 2d 32 00 00 00 00"
    " 00 00 00 00 00 03 00 7f 4b 6e 6f 62 20 31 2d 33 00 00 00 00 00 00 00 00 00 04 00 7f 4b 6e 6f 62"
    " 20 31 2d 34 00 00 00 00 00 00 00 00 00 05 00 7f 4b 6e 6f 62 20 32 2d 31 00 00 00 00 00 00 00 00"
    " 00 06 00 7f 4b 6e 6f 62 20 32 2d 32 00 00 00 00 00 00 00 00 00 07 00 7f 4b 6e 6f 62 20 32 2d 33"
    " 00 00 00 00 00 00 00 00 00 08 00 7f 4b 6e 6f 62 20 32 2d 34 00 00 00 00 00 00 00 00 0c f7")


def field(section, label):
    return next(f for f in FIELDS if f.section == section and f.label == label)


class Mpk3Test(unittest.TestCase):
    def setUp(self):
        self.p = parse_reply(REPLY, 1)

    def test_find_device_prefers_the_emulator(self):
        with tempfile.TemporaryDirectory() as run, mock.patch.dict(os.environ, {"XDG_RUNTIME_DIR": run}):
            os.environ.pop("MPK3_MIDI", None)
            os.mkdir(os.path.join(run, "mpk3-emu"))
            midi = os.path.join(run, "mpk3-emu", "midi")
            os.symlink("/dev/null", midi)
            with open(os.path.join(run, "mpk3-emu", "name"), "w") as f:
                f.write("MPK mini 3 custom\n")
            self.assertEqual(mpk3_cfg.find_device(), midi)
            with mock.patch("glob.glob", return_value=[]):  # stock tool: name doesn't match, no hardware either
                self.assertRaises(FileNotFoundError, find_device)
            os.environ["MPK3_MIDI"] = "/dev/x"
            self.assertEqual((find_device(), mpk3_cfg.find_device()), ("/dev/x", "/dev/x"))

    def test_parse_ignores_noise_and_realtime_bytes(self):
        noisy = b"\x90\x24\x40" + REPLY[:20] + b"\xf8" + REPLY[20:]  # note-on before, clock inside
        self.assertEqual(parse_reply(noisy, 1), self.p)
        self.assertIsNone(parse_reply(REPLY[:100], 1))
        self.assertIsNone(parse_reply(REPLY, 2))

    def test_decoded_values(self):
        self.assertEqual(len(self.p), PAYLOAD_LEN)
        self.assertEqual(field("General", "Program name").show(self.p), "Live")
        self.assertEqual(field("General", "Pad MIDI channel").show(self.p), "10")
        self.assertEqual(field("General", "Keys octave").show(self.p), "0")
        self.assertEqual(field("General", "Keys transpose").show(self.p), "0")
        self.assertEqual(field("Pad A1", "Note").show(self.p), "36 (C1)")
        self.assertEqual(field("Pad B8", "CC").get(self.p), 0x1F)
        self.assertEqual(field("Knob 8", "Name").show(self.p), "Knob 2-4")
        self.assertEqual(field("Knob 8", "CC").get(self.p), 8)

    def test_set_get_roundtrip_keeps_bytes(self):
        p = bytearray(self.p)
        for f in FIELDS:
            f.set(p, f.parse(f.show(p)))
        self.assertEqual(p, self.p)

    def test_validation(self):
        p = bytearray(self.p)
        with self.assertRaises(ValueError):
            field("Knob 1", "CC").set(p, 128)
        with self.assertRaises(ValueError):
            field("General", "Program name").set(p, "x" * 17)
        with self.assertRaises(ValueError):
            field("General", "Program name").set(p, "café")
        with self.assertRaises(ValueError):
            field("General", "Pad MIDI channel").set(p, field("General", "Pad MIDI channel").parse("17"))
        with self.assertRaises(ValueError):
            field("Knob 1", "CC").parse("abc")
        self.assertEqual(p, self.p)

    def test_edits(self):
        p = bytearray(self.p)
        tempo = field("Arpeggiator", "Tempo (BPM)")
        tempo.set(p, 200)
        self.assertEqual(tempo.get(p), 200)
        name = field("Knob 1", "Name")
        name.set(p, "Cutoff")
        self.assertEqual(name.get(p), "Cutoff")
        self.assertEqual(field("Knob 1", "Max").get(p), 127)  # neighbour untouched
        self.assertEqual(field("Arpeggiator", "Swing").parse("60%"), 10)

    def test_write_msg(self):
        msg = write_msg(3, self.p)
        self.assertEqual(len(msg), len(REPLY))
        self.assertEqual(msg[:8], bytes([0xF0, 0x47, 0x7F, 0x49, 0x64, 0x01, 0x76, 3]))
        self.assertEqual(msg[8:-1], self.p[1:])
        with self.assertRaises(ValueError):
            write_msg(1, self.p[:-1])


if __name__ == "__main__":
    unittest.main()
