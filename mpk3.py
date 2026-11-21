"""Akai MPK mini 3: read/write programs over USB SysEx via the raw ALSA MIDI device.

Protocol (community reverse-engineered, verified on hardware):
  query  F0 47 7F 49 66 00 01 <prog> F7
  reply  F0 47 00 49 67 01 76 <246-byte payload> F7
  write  F0 47 7F 49 64 01 76 <246-byte payload> F7
Program 0 is the live (RAM) working copy, 1-8 are the stored programs.
"""
import errno
import glob
import os
import select
import time
from dataclasses import dataclass

PAYLOAD_LEN = 246
REPLY_HDR = bytes([0xF0, 0x47, 0x00, 0x49, 0x67, 0x01, 0x76])
WRITE_HDR = bytes([0xF0, 0x47, 0x7F, 0x49, 0x64, 0x01, 0x76])


def query_msg(prog):
    return bytes([0xF0, 0x47, 0x7F, 0x49, 0x66, 0x00, 0x01, prog, 0xF7])


def write_msg(prog, payload):
    if len(payload) != PAYLOAD_LEN or any(b > 0x7F for b in payload):
        raise ValueError("invalid program payload")
    return WRITE_HDR + bytes([prog]) + bytes(payload[1:]) + b"\xF7"


def parse_reply(buf, prog):
    """Extract the payload for `prog` from raw input that may also hold notes/clock."""
    buf = bytes(b for b in buf if b < 0xF8)  # realtime bytes may be interleaved inside SysEx
    start = buf.find(REPLY_HDR + bytes([prog]))
    if start < 0:
        return None
    body = buf[start + len(REPLY_HDR):start + len(REPLY_HDR) + PAYLOAD_LEN + 1]
    if len(body) < PAYLOAD_LEN + 1:
        return None
    if body[-1] != 0xF7:
        raise ValueError("malformed reply from device")
    return bytearray(body[:-1])


def find_device():
    for path in sorted(glob.glob("/proc/asound/card*/midi0")):
        with open(path) as f:
            if f.readline().strip() == "MPK mini 3":
                return f"/dev/snd/midiC{path.split('/')[3][4:]}D0"
    raise FileNotFoundError("MPK mini 3 not found - is it plugged in?")


class MPK:
    def __init__(self, path=None):
        self.path = path or find_device()

    def _open(self):
        # WirePlumber briefly grabs the port when probing; a DAW holding it keeps it busy.
        for i in range(10):
            try:
                return os.open(self.path, os.O_RDWR | os.O_NONBLOCK)
            except OSError as e:
                if e.errno != errno.EBUSY:
                    raise
                time.sleep(0.2)
        raise OSError(errno.EBUSY, "MIDI port busy - close apps using the MPK mini 3")

    @staticmethod
    def _drain(fd):
        try:
            while os.read(fd, 4096):
                pass
        except BlockingIOError:
            pass

    def _read(self, fd, prog, timeout=2.0):
        self._drain(fd)
        os.write(fd, query_msg(prog))
        buf, end = b"", time.monotonic() + timeout
        while time.monotonic() < end:
            if select.select([fd], [], [], 0.1)[0]:
                try:
                    buf += os.read(fd, 4096)
                except BlockingIOError:
                    pass
                payload = parse_reply(buf, prog)
                if payload is not None:
                    return payload
        raise TimeoutError(f"no reply for program {prog}")

    def read(self, prog):
        fd = self._open()
        try:
            return self._read(fd, prog)
        finally:
            os.close(fd)

    def write(self, prog, payload):
        """Write a program and verify by reading it back."""
        msg = write_msg(prog, payload)
        fd = self._open()
        try:
            os.write(fd, msg)
            time.sleep(0.3)
            if self._read(fd, prog) != msg[len(WRITE_HDR):-1]:
                raise IOError(f"read-back of program {prog} does not match what was written")
        finally:
            os.close(fd)


# ---- field table: offsets into the 246-byte payload ----

NOTES = "C C# D D# E F F# G G# A A# B".split()


@dataclass
class Field:
    section: str
    label: str
    off: int
    lo: int = 0
    hi: int = 127
    shift: int = 0          # displayed value = raw + shift
    suffix: str = ""
    choices: tuple = ()
    note: bool = False
    text: int = 0           # >0: ASCII text of this many bytes
    wide: bool = False      # 14-bit value over two bytes (MSB first)

    def __post_init__(self):
        if self.choices:
            self.hi = len(self.choices) - 1

    def get(self, p):
        if self.text:
            return p[self.off:self.off + self.text].split(b"\0")[0].decode("ascii", "replace")
        if self.wide:
            return p[self.off] << 7 | p[self.off + 1]
        return p[self.off]

    def set(self, p, v):
        if self.text:
            b = v.encode("ascii", "replace")
            if len(b) > self.text or not v.isascii() or not all(32 <= c < 127 for c in b):
                raise ValueError(f"up to {self.text} plain ASCII characters")
            p[self.off:self.off + self.text] = b.ljust(self.text, b"\0")
            return
        if not self.lo <= v <= self.hi:
            raise ValueError(f"range {self.show_raw(self.lo)} .. {self.show_raw(self.hi)}")
        if self.wide:
            p[self.off:self.off + 2] = bytes([v >> 7, v & 0x7F])
        else:
            p[self.off] = v

    def show_raw(self, v):
        if self.choices:
            return self.choices[v] if v < len(self.choices) else f"? ({v})"
        if self.note:
            return f"{v} ({NOTES[v % 12]}{v // 12 - 2})"
        return f"{v + self.shift}{self.suffix}"

    def show(self, p):
        return self.get(p) if self.text else self.show_raw(self.get(p))

    def parse(self, s):
        """Typed text -> raw value (inverse of the display)."""
        if self.text:
            return s
        s = s.strip()
        if self.note:
            s = s.split(" ")[0]  # accept "36 (C1)" as shown
        for i, c in enumerate(self.choices):
            if c.lower() == s.lower():
                return i
        try:
            return int(s.removesuffix(self.suffix)) - self.shift
        except ValueError:
            raise ValueError(f"not a valid value: {s!r}") from None


OFF_ON = ("Off", "On")
# shortcut: joystick mode labels and X-/X+ order are best guesses from defaults; verify on hardware.
JOY_MODES = ("Pitchbend", "CC (single)", "CC (dual)")


def _fields():
    g, a, j = "General", "Arpeggiator", "Joystick"
    f = [
        Field(g, "Program name", 1, text=16),
        Field(g, "Pad MIDI channel", 17, 0, 15, shift=1),
        Field(g, "Aftertouch", 18, choices=("Off", "Channel", "Polyphonic")),
        Field(g, "Keys MIDI channel", 19, 0, 15, shift=1),
        Field(g, "Keys octave", 20, 0, 8, shift=-4),
        Field(g, "Keys transpose", 245, 0, 24, shift=-12),
        Field(a, "Arp", 21, choices=OFF_ON),
        Field(a, "Mode", 22, choices=("Up", "Down", "Exclusive", "Inclusive", "Order", "Random")),
        Field(a, "Time division", 23, choices=("1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T")),
        Field(a, "Clock", 24, choices=("Internal", "External")),
        Field(a, "Latch", 25, choices=OFF_ON),
        Field(a, "Swing", 26, 0, 25, shift=50, suffix="%"),
        Field(a, "Octaves", 30, 0, 3, shift=1),
        Field(a, "Tempo taps", 27, 2, 4),
        Field(a, "Tempo (BPM)", 28, 60, 240, wide=True),
        Field(j, "X mode", 31, choices=JOY_MODES),
        Field(j, "X- CC", 32),
        Field(j, "X+ CC", 33),
        Field(j, "Y mode", 34, choices=JOY_MODES),
        Field(j, "Y- CC", 35),
        Field(j, "Y+ CC", 36),
    ]
    for i in range(16):
        s, o = f"Pad {'AB'[i // 8]}{i % 8 + 1}", 37 + 3 * i
        f += [Field(s, "Note", o, note=True), Field(s, "Program change", o + 1), Field(s, "CC", o + 2)]
    for i in range(8):
        s, o = f"Knob {i + 1}", 85 + 20 * i
        f += [Field(s, "Name", o + 4, text=16),
              Field(s, "Mode", o, choices=("Absolute", "Relative")),
              Field(s, "CC", o + 1), Field(s, "Min", o + 2), Field(s, "Max", o + 3)]
    return f


FIELDS = _fields()
