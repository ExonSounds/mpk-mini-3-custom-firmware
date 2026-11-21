"""Cave Glide: fly a glider through a cave that scrolls past and keeps narrowing; turn knob 1 to climb or dive."""
import random

from mpk3_remote import idle_controls, pressed

GX = 18                                        # glider's x on the screen
GLIDER = ["##   ", "#### ", " ####", "#### ", "##   "]  # 5 x 5, nose to the right
RESTART_PAD = 7                                # PAD 8


class Cave:
    TITLE = "Cave Glide"
    INFO = "Fly through an endless cave that keeps getting narrower and faster. Distance is your score."
    CONTROLS = "Knob 1: climb / dive   PAD 8: fly again"

    def __init__(self, seed=None):
        self.rng, self.best = random.Random(seed), 0
        self.reset()

    def reset(self):
        self.mid, self.cols = 16.0, []          # cave columns: (top, bottom) = first and last open row
        self.dist, self.speed, self.scroll = 0.0, 28.0, 0.0
        self.y, self.state, self.pads = 16.0, "flying", (0,) * 8
        self.pillar_in = 90
        while len(self.cols) < 128:
            self._new_column()

    def _new_column(self):
        gap = max(9, 26 - int(self.dist / 120))  # narrows by a pixel every 120 px flown
        self.mid += self.rng.uniform(-0.9, 0.9)
        self.mid = min(max(self.mid, gap / 2 + 1), 31 - gap / 2)
        top, bottom = int(self.mid - gap / 2), int(self.mid + gap / 2)
        self.pillar_in -= 1
        if self.pillar_in == 2:                 # a 3-wide stalactite or stalagmite into the gap, one side per pillar
            self.pillar_top = self.rng.random() < 0.5
        if self.pillar_in <= 2:
            if self.pillar_top:
                top += gap // 2 - 1
            else:
                bottom -= gap // 2 - 1
            if self.pillar_in == 0:
                self.pillar_in = self.rng.randint(45, 90)
        self.cols.append((top, bottom))

    def step(self, ctrl, dt):
        tapped = pressed(self.pads, ctrl.pads)
        self.pads = ctrl.pads
        if self.state == "crashed":
            if RESTART_PAD in tapped:
                self.reset()
            return
        target = 2 + (127 - ctrl.knobs[0]) * 27 / 127  # knob up = glider up
        self.y += (target - self.y) * min(1.0, 10 * dt)
        self.speed = min(70.0, self.speed + 1.2 * dt)
        self.scroll += self.speed * dt
        while self.scroll >= 1:
            self.scroll -= 1
            self.dist += 1
            self.cols.pop(0)
            self._new_column()
        y = int(round(self.y))
        for dx in range(5):                      # the glider's 5 rows against the cave at its columns
            top, bottom = self.cols[GX + dx]
            if y - 2 < top or y + 2 > bottom:
                self.state, self.best = "crashed", max(self.best, int(self.dist / 10))
                return

    def render(self, scr):
        scr.clear()
        for x, (top, bottom) in enumerate(self.cols):
            scr.rect(x, 0, 1, top)
            scr.rect(x, bottom + 1, 1, 31 - bottom)
        y = int(round(self.y))
        for j, row in enumerate(GLIDER):
            for i, ch in enumerate(row):
                if ch == "#":
                    scr.px(GX + i, y - 2 + j)
        score = str(int(self.dist / 10))
        scr.label(127 - 6 * len(score), 1, score)
        if self.state == "crashed":
            scr.clear()
            scr.text(0, 0, f"CRASHED  {int(self.dist / 10)} M")
            scr.text(0, 11, f"BEST {self.best} M")
            scr.text(0, 24, "PAD 8: FLY AGAIN")

    def autopilot(self):
        """Knob 1 position that aims at the opening shared by the next 30 columns (preview and tests)."""
        top = bottom = None
        for t, b in self.cols[GX:GX + 30]:
            nt, nb = max(t, top if top is not None else t), min(b, bottom if bottom is not None else b)
            if nb - nt < 5:
                break
            top, bottom = nt, nb
        mid = (top + bottom) / 2
        return idle_controls(knobs=(max(0, min(127, round(127 - (mid - 2) * 127 / 27))),) + (64,) * 7)

    @classmethod
    def preview(cls, scr):
        g = cls(seed=3)
        for _ in range(150):
            g.step(g.autopilot(), 1 / 30)
        g.render(scr)
