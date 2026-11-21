"""Configure the MPK mini 3 *custom* firmware over USB-MIDI SysEx (protocol: fw/cfg.h).

usage: python3 mpk3_cfg.py info | get | set KEY=VALUE ... | save | defaults
"""
import glob
import os
import select
import sys
import time
from dataclasses import dataclass

from mpk3 import MPK

HDR = bytes([0xF0, 0x7D, 0x4D, 0x4B])
LAYOUT = 4  # fw/settings.h SETTINGS_LAYOUT
HELLO, GET, SET, SAVE, DEFAULTS = 1, 2, 3, 4, 5
STATUS = {0: "ok", 1: "wrong length", 2: "invalid values (or saving failed)", 0x7F: "unknown command"}


def msg(cmd, data=b""):
    return HDR + bytes([cmd]) + bytes(data) + b"\xF7"


def to_nibbles(raw):
    return bytes(x for v in raw for x in (v >> 4, v & 15))


def from_nibbles(d):
    if len(d) % 2 or any(x > 15 for x in d):
        raise ValueError("malformed settings reply")
    return bytes(d[i] << 4 | d[i + 1] for i in range(0, len(d), 2))


def parse_reply(buf, cmd):
    """Payload of the reply to `cmd` from raw input that may also hold notes/clock, or None if incomplete."""
    buf = bytes(b for b in buf if b < 0xF8)  # realtime bytes may be interleaved
    start = buf.find(HDR + bytes([cmd | 0x40]))
    if start < 0:
        return None
    end = buf.find(b"\xF7", start)
    return None if end < 0 else buf[start + 5:end]


CURVES = ("Soft", "Normal", "Hard", "Fixed 127")
NOTES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")


@dataclass
class Setting:
    key: str
    label: str
    group: str           # GUI tab; "" = not shown
    lo: int = 0
    hi: int = 127
    choices: tuple = ()
    shift: int = 0       # shown value = raw + shift
    kind: str = ""       # "note": also show the note name
    row: str = ""        # set for table cells (pads, knobs): row label ...
    col: str = ""        # ... and column label

    def __post_init__(self):
        if self.choices:
            self.hi = len(self.choices) - 1

    def text(self, v):
        """Raw value -> what an edit field shows (choice name or shifted number)."""
        return self.choices[v] if self.choices else str(v + self.shift)

    def parse(self, s):
        """Edit field text -> raw value (inverse of text()); ValueError if not valid."""
        if self.choices:
            if s not in self.choices:
                raise ValueError(f"{self.label}: choose one of {', '.join(self.choices)}")
            return self.choices.index(s)
        try:
            v = int(s.strip()) - self.shift
        except ValueError:
            raise ValueError(f"{self.label}: {s!r} is not a number") from None
        if not self.lo <= v <= self.hi:
            raise ValueError(f"{self.label}: {s.strip()} is outside {self.lo + self.shift}..{self.hi + self.shift}")
        return v

    def show(self, v):
        if self.choices:
            return self.choices[v]
        if self.kind == "note":
            return f"{v} ({NOTES[v % 12]}{v // 12 - 2})"
        return str(v + self.shift)


JOY_MODES = ("Pitch bend", "One CC (centre 64)", "CC per direction")
PADS = [f"{bank}{n}" for bank in "AB" for n in range(1, 9)]  # Akai numbering: bottom row 1-4, top row 5-8

# One entry per settings_t byte, in struct order (test_mpk3_cfg.py checks this against fw/settings.h).
SETTINGS = [
    Setting("keys_ch", "Keys channel", "General", 0, 15, shift=1),
    Setting("pads_ch", "Pads channel", "Pads", 0, 15, shift=1),
    Setting("ctrl_ch", "Knobs / joystick channel", "Knobs", 0, 15, shift=1),
    Setting("transpose", "Transpose", "General", 0, 24, shift=-12),
    Setting("key_curve", "Key velocity curve", "General", choices=CURVES),
    Setting("pad_curve", "Pad velocity curve", "Pads", choices=CURVES),
    Setting("pad_at", "Pad aftertouch", "Pads", choices=("Off", "Channel", "Polyphonic")),
    Setting("knob_speed", "Knob speed", "Knobs", choices=("Slow", "Normal", "Fast")),
    Setting("joy_x_mode", "X mode", "Joystick", choices=JOY_MODES),
    Setting("joy_x_neg", "X left CC (per direction)", "Joystick"),
    Setting("joy_x_pos", "X right CC (also the one-CC mode)", "Joystick"),
    Setting("joy_y_mode", "Y mode", "Joystick", choices=JOY_MODES),
    Setting("joy_y_neg", "Y down CC (per direction)", "Joystick"),
    Setting("joy_y_pos", "Y up CC (also the one-CC mode)", "Joystick"),
    Setting("arp_mode", "Arp mode", "Arp & tempo",
            choices=("Up", "Down", "Up/down exclusive", "Up/down inclusive", "As played", "Random")),
    Setting("arp_div", "Arp / repeat rate", "Arp & tempo",
            choices=("1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T")),
    Setting("arp_oct", "Arp octaves", "Arp & tempo", 0, 3, shift=1),
    Setting("arp_latch", "Arp latch", "Arp & tempo", choices=("Off", "On")),
    Setting("tempo", "Tempo (BPM)", "Arp & tempo", 40, 240),
    Setting("swing", "Swing (%)", "Arp & tempo", 50, 75),
    Setting("gate", "Gate (% of a step)", "Arp & tempo", 5, 100),
    Setting("screen_off", "Idle after (nothing used)", "Display", choices=("5 s", "10 s", "30 s", "60 s", "Never")),
    Setting("clock_ext", "Clock", "Arp & tempo", choices=("Internal", "External MIDI clock")),
    *[Setting(f"knob_cc{i}", f"Knob {i} CC", "Knobs", row=f"Knob {i}", col="CC") for i in range(1, 9)],
    *[Setting(f"knob_min{i}", f"Knob {i} min", "Knobs", row=f"Knob {i}", col="Min") for i in range(1, 9)],
    *[Setting(f"knob_max{i}", f"Knob {i} max", "Knobs", row=f"Knob {i}", col="Max") for i in range(1, 9)],
    *[Setting(f"knob_rel{i}", f"Knob {i} mode", "Knobs", choices=("Absolute", "Relative (64 +/- steps)"),
              row=f"Knob {i}", col="Mode") for i in range(1, 9)],
    *[Setting(f"pad_note{i}", f"Pad {p} note", "Pads", kind="note", row=f"Pad {p}", col="Note")
      for i, p in enumerate(PADS, 1)],
    *[Setting(f"pad_cc{i}", f"Pad {p} CC", "Pads", row=f"Pad {p}", col="CC (CC mode)") for i, p in enumerate(PADS, 1)],
    *[Setting(f"pad_pc{i}", f"Pad {p} program", "Pads", row=f"Pad {p}", col="Program (PROG CHANGE mode)")
      for i, p in enumerate(PADS, 1)],
    Setting("idle_screen", "When idle, show", "Display", choices=("Ping-pong", "Screen off")),
]


def decode(raw):
    if len(raw) != len(SETTINGS):
        raise ValueError(f"settings are {len(raw)} bytes, expected {len(SETTINGS)}")
    return {s.key: v for s, v in zip(SETTINGS, raw)}


def encode(values):
    out = []
    for s in SETTINGS:
        v = values[s.key]
        if not isinstance(v, int) or not s.lo <= v <= s.hi:
            raise ValueError(f"{s.label}: {v!r} is outside {s.lo}..{s.hi}")
        out.append(v)
    return bytes(out)


def find_device():
    for path in sorted(glob.glob("/proc/asound/card*/midi0")):
        with open(path) as f:
            if f.readline().strip() == "MPK mini 3 custom":
                return f"/dev/snd/midiC{path.split('/')[3][4:]}D0"
    raise FileNotFoundError("MPK mini 3 with custom firmware not found - is it plugged in and running it?")


class Device(MPK):
    """The custom firmware's config protocol; reuses MPK's port handling (busy retry, drain)."""

    def __init__(self, path=None):
        self.path = path or find_device()

    def request(self, cmd, data=b"", timeout=2.0):
        fd = self._open()
        try:
            self._drain(fd)
            os.write(fd, msg(cmd, data))
            buf, end = b"", time.monotonic() + timeout
            while time.monotonic() < end:
                if select.select([fd], [], [], 0.05)[0]:
                    try:
                        buf += os.read(fd, 4096)
                    except BlockingIOError:
                        continue
                    reply = parse_reply(buf, cmd)
                    if reply is not None:
                        return reply
            raise TimeoutError("no reply from the keyboard - is it running the custom firmware v8 or newer?")
        finally:
            os.close(fd)

    def _status(self, cmd, data=b""):
        r = self.request(cmd, data, timeout=3.0)
        if not r or r[0] != 0:
            raise IOError(f"keyboard said: {STATUS.get(r[0] if r else -1, 'no status')}")

    def hello(self):
        r = self.request(HELLO)
        if len(r) != 4:
            raise ValueError("malformed HELLO reply")
        info = {"version": f"{r[0]:x}.{r[1]:02x}", "layout": r[2], "size": r[3]}
        if info["layout"] != LAYOUT or info["size"] != len(SETTINGS):
            raise IOError(f"firmware settings layout {info['layout']} ({info['size']} bytes); this tool knows "
                          f"layout {LAYOUT} ({len(SETTINGS)} bytes) - update the firmware or this tool")
        return info

    def read(self):
        return decode(from_nibbles(self.request(GET)))

    def write(self, values):
        self._status(SET, to_nibbles(encode(values)))

    def save(self):
        self._status(SAVE)

    def defaults(self):
        self._status(DEFAULTS)


def main(argv):
    if not argv or argv[0] not in ("info", "get", "set", "save", "defaults"):
        sys.exit(__doc__)
    d = Device()
    info = d.hello()
    if argv[0] == "info":
        print(f"custom firmware v{info['version']}, settings layout {info['layout']}, {d.path}")
    elif argv[0] == "get":
        for s, v in zip(SETTINGS, encode(d.read())):
            print(f"{s.key:12s} {v:4d}  {s.label}: {s.show(v)}")
    elif argv[0] == "set":
        values = d.read()
        for a in argv[1:]:
            k, _, v = a.partition("=")
            if k not in values:
                sys.exit(f"unknown setting {k!r}")
            values[k] = int(v)
        d.write(values)
        print("applied (not saved; run: save)")
    elif argv[0] == "save":
        d.save()
        print("saved")
    else:
        d.defaults()
        print("defaults applied (not saved)")


if __name__ == "__main__":
    main(sys.argv[1:])
