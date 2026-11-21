"""Beat Lanes: beats slide along four lanes towards a line; hit the lane's pad (1-4) as each beat crosses it."""
import random

from mpk3_remote import idle_controls, pressed

HIT_X = 14                     # the line beats are hit on
SPEED = 45.0                   # px per second
PERFECT, GOOD = 0.05, 0.12     # timing windows in seconds
BPM = 112
LEAD_IN = 3.0                  # seconds before the first beat reaches the line
RESTART_PAD = 7                # PAD 8


def make_chart(rng, bars=16):
    """[time, lane] on a 1/8 grid, getting busier with every 4 bars."""
    beat, chart = 60 / BPM, []
    for bar in range(bars):
        busy = 0.3 + 0.12 * (bar // 4)
        for eighth in range(8):
            if eighth % 2 == 0 and eighth % 4 == 0 or rng.random() < busy:
                chart.append([LEAD_IN + (bar * 8 + eighth) * beat / 2, rng.randrange(4)])
    return chart


class Beats:
    TITLE = "Beat Lanes"
    INFO = "Beats slide in along four lanes. Hit the matching pad as each one crosses the line; keep the combo going."
    CONTROLS = "PAD 1-4: hit lanes 1-4 (top to bottom)   PAD 8: play again"

    def __init__(self, seed=None):
        self.rng, self.best = random.Random(seed), 0
        self.reset()

    def reset(self):
        self.chart = make_chart(self.rng)
        self.total = len(self.chart)
        self.clock, self.score, self.hits, self.combo, self.top_combo = 0.0, 0, 0, 0, 0
        self.pads, self.word, self.word_t, self.state = (0,) * 8, "", 0.0, "playing"
        self.lit = [0.0] * 4

    def _judge(self, word, points):
        self.word, self.word_t = word, 0.6
        if points:
            self.hits, self.combo = self.hits + 1, self.combo + 1
            self.top_combo = max(self.top_combo, self.combo)
            self.score += points * (1 + self.combo // 10)
        else:
            self.combo = 0

    def step(self, ctrl, dt):
        tapped = pressed(self.pads, ctrl.pads)
        self.pads = ctrl.pads
        self.word_t = max(0.0, self.word_t - dt)
        self.lit = [max(0.0, t - dt) for t in self.lit]
        if self.state == "done":
            if RESTART_PAD in tapped:
                self.reset()
            return
        self.clock += dt
        for lane in (p for p in tapped if p < 4):
            self.lit[lane] = 0.15
            near = [b for b in self.chart if b[1] == lane and abs(b[0] - self.clock) <= GOOD]
            if near:
                b = min(near, key=lambda b: abs(b[0] - self.clock))
                self.chart.remove(b)
                self._judge("PERFECT" if abs(b[0] - self.clock) <= PERFECT else "GOOD", 2 if abs(b[0] - self.clock) <= PERFECT else 1)
        for b in [b for b in self.chart if b[0] < self.clock - GOOD]:
            self.chart.remove(b)
            self._judge("MISS", 0)
        if not self.chart and self.clock > LEAD_IN:
            self.state, self.best = "done", max(self.best, self.score)

    def render(self, scr):
        scr.clear()
        for lane in range(4):
            y = lane * 8
            scr.text(0, y, str(lane + 1), on=True)
            if self.lit[lane]:
                scr.rect(HIT_X - 2, y, 5, 8)
        for y in range(0, 32, 2):
            scr.px(HIT_X, y)
        for t, lane in self.chart:
            x = HIT_X + (t - self.clock) * SPEED
            if -3 <= x < 128:
                scr.rect(int(x) - 2, lane * 8 + 1, 5, 6)
        scr.label(127 - 6 * len(str(self.score)), 1, str(self.score))
        if self.word_t:
            scr.label(127 - 6 * len(self.word), 24, self.word)
        elif self.combo >= 5:
            scr.label(127 - 6 * (len(str(self.combo)) + 1), 24, f"X{self.combo}")
        if self.state == "done":
            scr.clear()
            scr.text(0, 0, f"SCORE {self.score}  BEST {self.best}")
            scr.text(0, 11, f"HITS {self.hits}/{self.total} COMBO {self.top_combo}")
            scr.text(0, 24, "PAD 8: PLAY AGAIN")

    @classmethod
    def preview(cls, scr):
        g = cls(seed=7)
        for _ in range(int((LEAD_IN + 4) * 30)):           # an autopilot that hits every beat
            pads = [0] * 8
            for t, lane in g.chart:
                if abs(t - g.clock) < 0.02:
                    pads[lane] = 100
            g.step(idle_controls(pads=tuple(pads)), 1 / 30)
        g.render(scr)
