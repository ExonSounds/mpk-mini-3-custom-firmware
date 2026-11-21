"""Audio Visualiser: whatever this computer is playing, shown on the keyboard's screen.

Records the default output's monitor (everything you hear) with parec or pw-record; numpy does the FFT.
"""
import fcntl
import os
import shutil
import subprocess

import numpy as np

from mpk3_cfg import NOTES
from mpk3_remote import idle_controls, pressed

RATE, N = 44100, 2048                     # sample rate, FFT size (~46 ms)
BANDS = 32                                # spectrum bars, 4 px each
EDGES = np.logspace(np.log10(40), np.log10(16000), BANDS + 1)
BIN_OF = np.clip((EDGES / (RATE / N)).astype(int), 1, N // 2)
CAPTURE = [  # first one installed wins; both give raw 16-bit mono at RATE on stdout
    ["parec", "-d", "@DEFAULT_MONITOR@", "--format=s16le", f"--rate={RATE}", "--channels=1", "--raw",
     "--latency-msec=20"],
    ["pw-record", "-P", "{ stream.capture.sink=true }", "--format=s16", f"--rate={RATE}", "--channels=1", "-"],
]
MODES = ("SPECTRUM", "WAVE", "WATERFALL")  # PAD 1, 2, 3
FOLLOW_PAD, CYCLES = 3, 2                  # PAD 4 toggles pitch follow: the waveform then shows 2 cycles
MIN_F, MAX_F = 50, 1500                    # pitch range it looks for, Hz
DITHER = np.tile((np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]) + 0.5) / 16, (8, 32))
WEIGHTS = (1 << np.arange(8)).reshape(1, 8, 1)


def detect_period(x):
    """Fundamental period in samples (fractional), or None without a clear pitch (noise, drums, silence)."""
    x = x - x.mean()
    if np.sqrt(np.mean(x ** 2)) < 1e-3:
        return None
    r = np.fft.irfft(np.abs(np.fft.rfft(x, 2 * len(x))) ** 2)[:len(x)]
    r /= len(x) - np.arange(len(x))                          # unbiased: long lags aren't penalised
    lo, hi = int(RATE / MAX_F), int(RATE / MIN_F)
    best = r[lo:hi].max()
    if best < 0.5 * r[0]:
        return None
    for i in range(lo, hi):                                  # the first peak nearly as high as the best one,
        if r[i] >= 0.9 * best and r[i] >= r[i - 1] and r[i] >= r[i + 1]:  # so octaves below don't win
            a, b, c = r[i - 1], r[i], r[i + 1]
            return i + 0.5 * (a - c) / (a - 2 * b + c) if a - 2 * b + c else float(i)
    return None


def note_name(period):
    m = round(69 + 12 * np.log2(RATE / period / 440))
    return f"{NOTES[m % 12]}{m // 12 - 2}"               # same octave numbers as the keyboard (C3 = 60)


def pack(img):
    """32 x 128 booleans -> the screen's 512 bytes (8 rows per byte, bit 0 on top)."""
    return (img.reshape(4, 8, 128) * WEIGHTS).sum(axis=1).astype(np.uint8).tobytes()


class Visualiser:
    TITLE = "Audio Visualiser"
    INFO = ("Shows whatever this computer is playing: a spectrum with falling peaks, a steady waveform, or a "
            "scrolling waterfall.")
    CONTROLS = ("PAD 1: spectrum   PAD 2: waveform (edges = 0 dB with knob 1 centred)   PAD 3: waterfall   "
                "PAD 4: pitch follow on/off   Knob 1: sensitivity / zoom")

    def __init__(self, capture=True):
        self.samples = np.zeros(N, dtype=np.float32)
        self.level = np.zeros(BANDS)           # bar heights in px, smoothed
        self.peak, self.peak_t = np.zeros(BANDS), np.zeros(BANDS)
        self.fall = np.zeros((32, 128))         # waterfall intensities 0..1, newest column on the right
        self.mode, self.note, self.note_t, self.gain_db, self.quiet = 0, "", 0.0, 0.0, 0.0
        self.pads, self.knob, self.proc, self.buf = (0,) * 8, None, None, b""
        self.follow, self.period = False, None
        if capture:
            self._start()

    def _start(self):
        cmd = next((c for c in CAPTURE if shutil.which(c[0])), None)
        if cmd is None:
            raise FileNotFoundError("no audio recorder found - install pipewire (pw-record) or pulseaudio-utils "
                                    "(parec)")
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        fd = self.proc.stdout.fileno()
        fcntl.fcntl(fd, fcntl.F_SETFL, fcntl.fcntl(fd, fcntl.F_GETFL) | os.O_NONBLOCK)

    def feed(self, data):
        """Raw 16-bit little-endian samples in; keeps the newest N (also used by the tests)."""
        data = self.buf + data
        whole = len(data) // 2 * 2
        self.buf = data[whole:]
        new = np.frombuffer(data[:whole], dtype="<i2").astype(np.float32) / 32768
        self.samples = np.concatenate((self.samples, new))[-N:]

    def step(self, ctrl, dt):
        if self.proc:
            try:
                while chunk := os.read(self.proc.stdout.fileno(), 65536):
                    self.feed(chunk)
            except BlockingIOError:
                pass
        for p in pressed(self.pads, ctrl.pads):
            if p < 3:
                self.mode, self.note, self.note_t = p, MODES[p], 1.0
            elif p == FOLLOW_PAD:
                self.follow = not self.follow
                self.note, self.note_t = f"PITCH FOLLOW {'ON' if self.follow else 'OFF'}", 1.0
                if self.follow:
                    self.mode = 1                            # it belongs to the waveform view
        self.period = detect_period(self.samples) if self.follow and self.mode == 1 else None
        self.pads = ctrl.pads
        self.gain_db = (ctrl.knobs[0] - 64) * 0.4   # knob 1: about -26 .. +25 dB
        if self.knob is not None and ctrl.knobs[0] != self.knob:  # the waveform's edges move with the gain
            self.note = f"EDGE {-self.gain_db:+.0f} DB" if self.mode == 1 else f"GAIN {self.gain_db:+.0f} DB"
            self.note_t = 1.0
        self.knob = ctrl.knobs[0]
        self.note_t = max(0.0, self.note_t - dt)

        mag = np.abs(np.fft.rfft(self.samples * np.hanning(N))) / (N / 4)  # full-scale sine = 1
        bands = np.array([mag[a:max(b, a + 1)].max() for a, b in zip(BIN_OF[:-1], BIN_OF[1:])])
        db = 20 * np.log10(bands + 1e-9) + self.gain_db
        target = np.clip((db + 60) / 54 * 32, 0, 32)                     # -60 dB .. -6 dB fills the height
        self.level = np.maximum(target, self.level - 70 * dt)             # rise at once, fall smoothly
        held = self.level >= self.peak
        self.peak = np.where(held, self.level, np.where(self.peak_t > 0, self.peak, self.peak - 30 * dt))
        self.peak_t = np.where(held, 0.5, self.peak_t - dt)
        self.fall = np.roll(self.fall, -1, axis=1)
        self.fall[:, -1] = target[::-1] / 32                              # low notes at the bottom
        self.quiet = self.quiet + dt if np.sqrt(np.mean(self.samples ** 2)) < 1e-4 else 0.0

    def render(self, scr):
        img = np.zeros((32, 128), dtype=bool)
        if self.mode == 0:                                  # spectrum: 3 px bars with peak dots
            cols = np.arange(128)
            img |= np.arange(32).reshape(32, 1) >= 32 - np.repeat(self.level.round(), 4)
            peak = np.repeat(self.peak, 4)
            show = (cols % 4 != 3) & (peak > 1)
            img[np.clip(31 - peak.round(), 0, 31).astype(int)[show], cols[show]] = True
            img[:, 3::4] = False
        elif self.mode == 1:                                # waveform, steadied on a rising zero crossing
            s = self.samples * 10 ** (self.gain_db / 20)    # knob 1 centred: 0 dBFS peaks touch top and bottom
            width = CYCLES * self.period if self.period else 1024  # follow: 2 cycles; else 23 ms
            last = int(N - width) - 1
            start = next((i for i in range(last, 0, -1) if s[i - 1] < 0 <= s[i]), last)
            wave = np.interp(start + np.arange(128) * width / 128, np.arange(N), s)
            y = np.clip(np.round(15.5 - wave * 15.5), 0, 31).astype(int)
            for x in range(128):                            # join the dots so steep edges stay visible
                lo, hi = sorted((y[x], y[x - 1] if x else y[x]))
                img[lo:hi + 1, x] = True
        else:                                               # waterfall, brightness by ordered dithering
            img = self.fall > DITHER
        scr.fb[:] = pack(img)
        if self.note_t:
            scr.label(1, 1, self.note)
        elif self.period:
            name = note_name(self.period)
            scr.label(127 - 6 * len(name), 1, name)
        elif self.quiet > 2:
            scr.label(1, 1, "PLAY SOME AUDIO")

    def close(self):
        if self.proc:
            self.proc.terminate()
            self.proc.wait(2)
            self.proc.stdout.close()
            self.proc = None

    @classmethod
    def preview(cls, scr):
        g = cls(capture=False)
        t = np.arange(N) / RATE
        tone = sum(a * np.sin(2 * np.pi * f * t) for f, a in ((80, 0.5), (220, 0.3), (660, 0.15), (2500, 0.06),
                                                                 (7000, 0.03)))
        g.feed((tone * 32767 * 0.8).astype("<i2").tobytes())
        for _ in range(3):
            g.step(idle_controls(), 1 / 30)
        g.render(scr)
