"""Turbo Loop: top-down racing, three laps against the clock (the world turns around the car)."""
import math

from mpk3_remote import Controls, Screen

W, H = 640, 400                   # world size in pixels (one screen pixel = one world pixel)
ROAD, KERB = 11, 2                # half width of the tarmac, kerb width
LAPS = 3
CAR_X, CAR_Y = 34, 16             # the car on the screen, nose to the right; the world turns around it
GRASS, TARMAC, KERB_A, KERB_B, LINE = 0, 1, 2, 3, 4
TRACK = [(80, 200), (110, 80), (250, 50), (330, 130), (420, 60), (570, 90), (590, 250), (480, 330), (330, 290),
         (220, 350), (100, 330)]  # control points of the closed loop, clockwise
GAS_PAD, REVERSE_PAD = 7, 3       # PAD 8, PAD 4 (Akai numbering: bottom row 1-4, top row 5-8)
CAR = [" ##   ## ",
       "#########",
       "####  ###",
       "#########",
       " ##   ## "]


def spline(points, step=1.0):
    """Closed Catmull-Rom curve through the points, sampled about every `step` pixels."""
    out, n = [], len(points)
    for i in range(n):
        p0, p1, p2, p3 = (points[(i + k) % n] for k in (-1, 0, 1, 2))
        steps = max(1, int(math.dist(p1, p2) / step))
        for s in range(steps):
            t = s / steps
            out.append(tuple(0.5 * (2 * b + (-a + c) * t + (2 * a - 5 * b + 4 * c - d) * t * t +
                                    (-a + 3 * b - 3 * c + d) * t ** 3) for a, b, c, d in zip(p0, p1, p2, p3)))
    return out


def build_world(points=TRACK):
    """(grid, path): grid[y * W + x] = GRASS / TARMAC / KERB_A / KERB_B / LINE; path = the centre line."""
    path, grid = spline(points), bytearray(W * H)

    def disc(cx, cy, r, val):
        cx, cy = int(cx), int(cy)
        for dy in range(-r, r + 1):
            y = cy + dy
            if 0 <= y < H:
                dx = int(math.sqrt(r * r - dy * dy))
                x0, x1 = max(cx - dx, 0), min(cx + dx + 1, W)
                if x1 > x0:
                    grid[y * W + x0:y * W + x1] = bytes([val]) * (x1 - x0)

    for i, (x, y) in enumerate(path):  # kerbs first, striped along the track; the tarmac goes on top
        disc(x, y, ROAD + KERB, KERB_A if i // 6 % 2 else KERB_B)
    for x, y in path:
        disc(x, y, ROAD, TARMAC)
    (x0, y0), (x1, y1) = path[0], path[3]  # start/finish line across the road at path[0]
    tx, ty = (x1 - x0) / math.dist(path[0], path[3]), (y1 - y0) / math.dist(path[0], path[3])
    for along in range(-2, 3):
        for side in range(-ROAD, ROAD + 1):
            gx, gy = int(x0 + along * tx - side * ty), int(y0 + along * ty + side * tx)
            if grid[gy * W + gx] == TARMAC:
                grid[gy * W + gx] = LINE
    return grid, path


def lap_time(t):
    return f"{int(t // 60)}:{t % 60:04.1f}"


class Race:
    TITLE = "Turbo Loop"
    INFO = "Top-down racing on a winding circuit. Three laps against the clock; grass slows you down."
    CONTROLS = "Joystick X: steer   PAD 8: gas (press harder)   PAD 4: brake / reverse"

    def __init__(self, world=None):
        self.grid, self.path = world or build_world()
        self.best = None
        self.reset()

    def reset(self):
        n = len(self.path)
        self.idx = n - 10                    # start a little behind the line, facing along the track
        self.x, self.y = self.path[self.idx]
        nx, ny = self.path[(self.idx + 3) % n]
        self.heading = math.atan2(ny - self.y, nx - self.x)
        self.v = 0.0
        self.progress = self.idx - n         # path samples driven; a lap is len(path)
        self.laps_done, self.lap_start = 0, 0.0
        self.clock, self.state = 0.0, "countdown"  # countdown -> racing -> finished
        self.finish_time, self.gas_released = None, False

    def cell(self, x, y):
        xi, yi = int(x), int(y)
        return self.grid[yi * W + xi] if 0 <= xi < W and 0 <= yi < H else GRASS

    def _follow(self):
        """Move idx to the nearest centre-line sample and count the progress (backwards counts negative)."""
        n, best, bi = len(self.path), None, self.idx
        for d in range(-40, 41):
            i = (self.idx + d) % n
            px, py = self.path[i]
            dd = (px - self.x) ** 2 + (py - self.y) ** 2
            if best is None or dd < best:
                best, bi = dd, i
        step = (bi - self.idx + n // 2) % n - n // 2
        self.idx, self.progress = bi, self.progress + step

    def step(self, ctrl, dt):
        """Advance by dt seconds with the keyboard's controls (mpk3_remote.Controls)."""
        self.clock += dt
        gas, rev = min(1.0, ctrl.pads[GAS_PAD] / 90), min(1.0, ctrl.pads[REVERSE_PAD] / 90)
        if self.state == "countdown":
            if self.clock >= 3.0:
                self.state, self.clock, self.lap_start = "racing", 0.0, 0.0
            return
        if self.state == "finished":
            self.v *= max(0.0, 1 - 2 * dt)
            self.gas_released |= gas == 0
            if self.gas_released and gas > 0:  # PAD 8 again: new race
                self.reset()
                return
            gas = rev = 0.0
        on_grass = self.cell(self.x, self.y) == GRASS
        if gas:
            self.v += 160 * gas * dt
        if rev:
            self.v -= (260 if self.v > 0 else 90) * rev * dt
        self.v -= self.v * (1.5 if on_grass else 0.5) * dt  # drag
        if not gas and not rev:
            self.v = max(0.0, self.v - 25 * dt) if self.v > 0 else min(0.0, self.v + 25 * dt)
        self.v = max(-45.0, min(self.v, 50.0 if on_grass and self.v > 50 else 130.0))
        steer = ctrl.joy_x / 1000
        if abs(steer) < 0.1:
            steer = 0.0
        self.heading += steer * 2.6 * min(1.0, abs(self.v) / 25) * (1 if self.v >= 0 else -1) * dt
        nx, ny = self.x + math.cos(self.heading) * self.v * dt, self.y + math.sin(self.heading) * self.v * dt
        if 0 <= nx < W and 0 <= ny < H:
            self.x, self.y = nx, ny
        else:
            self.v = 0.0  # the edge of the world
        self._follow()
        if self.state == "racing" and self.progress >= (self.laps_done + 1) * len(self.path):
            lap = self.clock - self.lap_start
            self.best = lap if self.best is None else min(self.best, lap)
            self.laps_done, self.lap_start = self.laps_done + 1, self.clock
            if self.laps_done == LAPS:
                self.state, self.finish_time = "finished", self.clock

    def render(self, scr):
        fb, grid = scr.fb, self.grid
        c, s = math.cos(self.heading), math.sin(self.heading)
        for sx in range(128):
            f = sx - CAR_X
            wx, wy = self.x + f * c + CAR_Y * s, self.y + f * s - CAR_Y * c  # world point at the top of the column
            for page in range(4):
                byte = 0
                for bit in range(8):
                    xi, yi = int(wx), int(wy)
                    v = grid[yi * W + xi] if 0 <= xi < W and 0 <= yi < H else GRASS
                    if (v == GRASS and (xi * 73856093 ^ yi * 19349663) % 13 == 0) or v == KERB_A or \
                            (v == KERB_B and (xi + yi) & 1) or (v == LINE and ((xi >> 1) + (yi >> 1)) & 1):
                        byte |= 1 << bit
                    wx, wy = wx - s, wy + c  # one pixel down the screen = one pixel to the car's right
                fb[page * 128 + sx] = byte
        scr.rect(CAR_X - 5, CAR_Y - 3, 11, 7, on=False)  # dark outline so the car shows on any ground
        for j, row in enumerate(CAR):
            for i, ch in enumerate(row):
                if ch == "#":
                    scr.px(CAR_X - 4 + i, CAR_Y - 2 + j)
        self._hud(scr)

    def autopilot(self):
        """Gas and steering towards the centre line ahead (preview and tests)."""
        tx, ty = self.path[(self.idx + 25) % len(self.path)]
        turn = (math.atan2(ty - self.y, tx - self.x) - self.heading + math.pi) % (2 * math.pi) - math.pi
        return Controls((0,) * 7 + (110,), max(-1000, min(1000, int(turn * 1500))), 0, 0, (0,) * 25, (0,) * 8)

    @classmethod
    def preview(cls, scr):
        g = cls()
        for _ in range(200):
            g.step(g.autopilot(), 1 / 30)
        g.render(scr)

    def _hud(self, scr):
        label = scr.label
        if self.state == "countdown":
            n = 3 - int(self.clock)
            label(1, 1, f"LAP 1/{LAPS}")
            scr.rect(54, 7, 20, 18, on=False)
            scr.text(58, 9, str(n), 2)
        elif self.state == "racing":
            label(1, 1, f"{min(self.laps_done + 1, LAPS)}/{LAPS}")
            t = lap_time(self.clock - self.lap_start)
            label(127 - 6 * len(t), 1, t)
            if self.clock < 0.8:
                scr.rect(50, 7, 28, 18, on=False)
                scr.text(52, 9, "GO", 2)
            elif self.best is not None:
                b = "BEST " + lap_time(self.best)
                label(127 - 6 * len(b), 24, b)
        else:
            scr.clear()
            scr.text(0, 0, "FINISH " + lap_time(self.finish_time))
            scr.text(0, 11, "BEST LAP " + lap_time(self.best))
            scr.text(0, 24, "PAD 8: RACE AGAIN")
