"""System Monitor: this computer's load, temperatures, memory and network on the keyboard's screen.

Reads /proc and /sys (hwmon sensors); an NVIDIA GPU through nvidia-smi, an AMD one through /sys.
"""
import fcntl
import glob
import os
import shutil
import subprocess
from collections import deque

from mpk3_remote import pressed

PAGES = ("OVERVIEW", "CORES", "HISTORY", "TEMPS")  # PAD 1-4
SAMPLE = 0.5                                       # seconds between readings
IGNORE_NET = ("lo", "docker", "veth", "br-", "virbr")
NVIDIA = ["nvidia-smi", "--query-gpu=utilization.gpu,temperature.gpu,memory.used,memory.total",
          "--format=csv,noheader,nounits", "-lms", "1000"]


def human(n):
    """Bytes -> '812B', '1.2K', '34M', '1.5G' (powers of 1024)."""
    for unit in "BKMGT":
        if n < 1000 or unit == "T":
            return f"{n:.0f}{unit}" if n >= 10 or unit == "B" else f"{n:.1f}{unit}"
        n /= 1024


def read(path, default=None):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return default


class Stats:
    """Samples the system; root lets the tests point it at a fake /proc and /sys."""

    def __init__(self, root="/", gpu=True):
        self.root = root
        self.cpu, self.cores, self.mem, self.net = 0.0, [], (0, 1), (0.0, 0.0)
        self.temps, self.fans, self.gpu = {}, [], None    # gpu: (util %, temp C, vram used MB, vram total MB)
        self._cpu_last, self._net_last = None, None
        self._nv, self._nv_buf = None, b""
        self.hwmon = {}                                   # sensor chip name -> its directory
        for d in sorted(glob.glob(self._p("sys/class/hwmon/hwmon*"))):
            self.hwmon.setdefault((read(os.path.join(d, "name"), "") or "").strip(), d)
        if gpu and shutil.which(NVIDIA[0]):
            self._nv = subprocess.Popen(NVIDIA, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            fd = self._nv.stdout.fileno()
            fcntl.fcntl(fd, fcntl.F_SETFL, fcntl.fcntl(fd, fcntl.F_GETFL) | os.O_NONBLOCK)

    def _p(self, rel):
        return os.path.join(self.root, rel)

    def _temp(self, chip, label=None):
        d = self.hwmon.get(chip)
        if not d:
            return None
        for t in sorted(glob.glob(os.path.join(d, "temp*_input"))):
            if label is None or (read(t.replace("_input", "_label"), "") or "").strip() == label:
                v = (read(t, "0") or "0").strip()
                return int(v) / 1000 if int(v) > 0 else None
        return None

    def sample(self, dt):
        lines = [ln.split() for ln in (read(self._p("proc/stat"), "") or "").splitlines() if ln.startswith("cpu")]
        now = [(sum(map(int, f[1:9])), int(f[4]) + int(f[5])) for f in lines]  # (total, idle + iowait) jiffies
        if self._cpu_last and len(self._cpu_last) == len(now):
            use = [100 * (1 - (i - li) / (t - lt)) if t > lt else 0.0
                   for (t, i), (lt, li) in zip(now, self._cpu_last)]
            self.cpu, self.cores = use[0], use[1:]
        self._cpu_last = now

        info = dict(ln.split(":", 1) for ln in (read(self._p("proc/meminfo"), "") or "").splitlines() if ":" in ln)
        kb = {k: int(v.split()[0]) * 1024 for k, v in info.items() if k in ("MemTotal", "MemAvailable")}
        if len(kb) == 2:
            self.mem = (kb["MemTotal"] - kb["MemAvailable"], kb["MemTotal"])

        rx = tx = 0
        for ln in (read(self._p("proc/net/dev"), "") or "").splitlines()[2:]:
            name, data = ln.split(":", 1)
            if not name.strip().startswith(IGNORE_NET):
                f = data.split()
                rx, tx = rx + int(f[0]), tx + int(f[8])
        if self._net_last and dt > 0:
            self.net = (max(0, rx - self._net_last[0]) / dt, max(0, tx - self._net_last[1]) / dt)
        self._net_last = (rx, tx)

        board = next((c for c in self.hwmon if c.startswith("nct") or c.startswith("it87")), None)
        self.temps = {"CPU": self._temp("k10temp", "Tctl") or self._temp("coretemp", "Package id 0")
                      or self._temp("zenpower"),
                      "SSD": self._temp("nvme"),
                      "BRD": self._temp(board, "SYSTIN") if board else None}
        self.fans = [int(v) for d in self.hwmon.values() for f in sorted(glob.glob(os.path.join(d, "fan*_input")))
                     for v in [(read(f, "0") or "0").strip()] if v.isdigit() and int(v) > 0]
        self._gpu()

    def _gpu(self):
        if self._nv:                                      # newest complete line from nvidia-smi
            try:
                while chunk := os.read(self._nv.stdout.fileno(), 4096):
                    self._nv_buf += chunk
            except BlockingIOError:
                pass
            *done, self._nv_buf = self._nv_buf.split(b"\n")
            for line in reversed(done):
                try:
                    self.gpu = tuple(float(x) for x in line.decode().split(","))[:4]
                    break
                except ValueError:
                    continue
        else:                                             # AMD: busy percent and amdgpu's sensor
            busy = glob.glob(self._p("sys/class/drm/card*/device/gpu_busy_percent"))
            if busy:
                self.gpu = (float(read(busy[0], "0") or 0), self._temp("amdgpu") or 0.0, 0.0, 0.0)
        if self.gpu:
            self.temps["GPU"] = self.gpu[1]

    def uptime(self):
        up = float((read(self._p("proc/uptime"), "0") or "0").split()[0])
        load = (read(self._p("proc/loadavg"), "0") or "0").split()[0]
        d, h, m = int(up // 86400), int(up % 86400 // 3600), int(up % 3600 // 60)
        return (f"{d}D " if d else "") + f"{h}:{m:02d}", load

    def close(self):
        if self._nv:
            self._nv.terminate()
            self._nv.wait(2)
            self._nv.stdout.close()
            self._nv = None


def bar(scr, x, y, w, frac):
    """A 5 px tall bar: dotted track, filled part."""
    for i in range(x, x + w, 2):
        scr.px(i, y + 4)
    scr.rect(x, y, int(round(max(0.0, min(1.0, frac)) * w)), 5)


class SysMon:
    TITLE = "System Monitor"
    INFO = "This computer's CPU and GPU load, temperatures, memory, network and fans, live on the keyboard."
    CONTROLS = "PAD 1: overview   PAD 2: CPU cores   PAD 3: history   PAD 4: temperatures and fans"

    def __init__(self, stats=None):
        self.stats = stats if stats is not None else Stats()
        self.page, self.note_t, self.pads, self.wait = 0, 0.0, (0,) * 8, SAMPLE
        self.history = deque([(0.0, 0.0)] * 128, maxlen=128)  # (cpu %, gpu %), one per SAMPLE: 64 s
        self.stats.sample(0)

    def step(self, ctrl, dt):
        for p in pressed(self.pads, ctrl.pads):
            if p < len(PAGES):
                self.page, self.note_t = p, 1.0
        self.pads = ctrl.pads
        self.note_t = max(0.0, self.note_t - dt)
        self.wait -= dt
        if self.wait <= 0:
            self.stats.sample(SAMPLE - self.wait)         # time since the last reading
            self.wait += SAMPLE
            s = self.stats
            self.history.append((s.cpu, s.gpu[0] if s.gpu else 0.0))

    def render(self, scr):
        s = self.stats
        scr.clear()
        temp = {k: (f"{v:.0f}C" if v else "--") for k, v in s.temps.items()}
        if self.page == 0:
            gpu = f"{s.gpu[0]:3.0f}% {temp.get('GPU', '--')}" if s.gpu else "  --"
            rows = (("CPU", s.cpu / 100, f"{s.cpu:3.0f}% {temp.get('CPU', '--')}"),
                    ("GPU", s.gpu[0] / 100 if s.gpu else 0, gpu),
                    ("RAM", s.mem[0] / s.mem[1], f"{s.mem[0] / 2**30:.1f}/{s.mem[1] / 2**30:.0f}G"))
            for i, (name, frac, value) in enumerate(rows):
                scr.text(0, i * 8, name)
                bar(scr, 20, i * 8 + 1, 48, frac)
                scr.text(72, i * 8, value)
            scr.text(0, 24, f"NET DN {human(s.net[0])} UP {human(s.net[1])}")
        elif self.page == 1:
            w = 128 // max(1, len(s.cores))
            for i, u in enumerate(s.cores):
                h = int(round(u / 100 * 32))
                scr.rect(i * w, 32 - h, max(1, w - 2), h)
                scr.px(i * w, 31)                         # a foot, so idle cores still show
            if self.note_t:
                scr.label(1, 1, f"CPU {s.cpu:.0f}% {len(s.cores)} THREADS")
        elif self.page == 2:
            for x, (cpu, gpu) in enumerate(self.history):  # CPU shaded, GPU as a 2 px line
                for y in range(31 - int(round(cpu / 100 * 31)), 32):
                    if (x + y) % 2 == 0:
                        scr.px(x, y)
                g = 31 - int(round(gpu / 100 * 31))
                scr.rect(x, max(0, g - 1), 1, 2)
            gpu = f" GPU{s.gpu[0]:3.0f}%" if s.gpu else ""
            scr.label(1, 1, f"CPU{s.cpu:3.0f}%{gpu}")
        else:
            up, load = s.uptime()
            scr.text(0, 0, f"CPU {temp.get('CPU', '--'):4s} GPU {temp.get('GPU', '--')}")
            scr.text(0, 8, f"SSD {temp.get('SSD', '--'):4s} BRD {temp.get('BRD', '--')}")
            scr.text(0, 16, "FAN " + ("/".join(map(str, s.fans[:3])) + " RPM" if s.fans else "--"))
            scr.text(0, 24, f"UP {up} LD {load}")
        if self.note_t and self.page != 1:
            scr.label(127 - 6 * len(PAGES[self.page]), 24, PAGES[self.page])

    def close(self):
        self.stats.close()

    @classmethod
    def preview(cls, scr):
        g = cls(stats=Stats(gpu=False))
        g.stats.cpu, g.stats.gpu, g.stats.net = 23.0, (41.0, 52.0, 1757.0, 8192.0), (1.3e6, 42e3)
        g.stats.temps["GPU"] = 52.0
        g.render(scr)
        g.close()
