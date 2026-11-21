"""Key Catch: notes drop above the 25 keys; play the key under a note as it reaches the bottom to catch it."""
import random

from mpk3_remote import idle_controls, pressed

KEY_W = 5                                   # 25 keys x 5 px = 125 px
ZONE = (21.0, 28.5)                         # a note can be caught while its top is in here
BLACK = {1, 3, 6, 8, 10}                    # semitones of the black keys
RESTART_PAD = 7                             # PAD 8


def key_x(k):
    return 1 + k * KEY_W


class KeyCatch:
    TITLE = "Key Catch"
    INFO = "Notes rain down onto the keyboard. Play the key right under each note as it lands. Three misses and out."
    CONTROLS = "Piano keys: catch   PAD 8: play again"

    def __init__(self, seed=None):
        self.rng, self.best = random.Random(seed), 0
        self.reset()

    def reset(self):
        self.notes, self.score, self.lives = [], 0, 3   # notes: [key, y]
        self.clock, self.next_drop, self.state = 0.0, 1.0, "playing"
        self.keys, self.pads, self.flash = (0,) * 25, (0,) * 8, {}

    def speed(self):
        return 9 + self.score * 0.35                    # px per second

    def step(self, ctrl, dt):
        hit, tapped = pressed(self.keys, ctrl.keys), pressed(self.pads, ctrl.pads)
        self.keys, self.pads = ctrl.keys, ctrl.pads
        self.flash = {k: t - dt for k, t in self.flash.items() if t > dt}
        if self.state == "over":
            if RESTART_PAD in tapped:
                self.reset()
            return
        self.clock += dt
        if self.clock >= self.next_drop:
            free = [k for k in range(25) if all(n[0] != k for n in self.notes)]
            self.notes.append([self.rng.choice(free), -3.0])
            self.next_drop = self.clock + max(0.45, 1.6 - self.score * 0.04)
        for n in self.notes:
            n[1] += self.speed() * dt
        for k in hit:
            caught = [n for n in self.notes if n[0] == k and ZONE[0] <= n[1] <= ZONE[1]]
            if caught:
                self.notes.remove(caught[0])
                self.score += 1
                self.flash[k] = 0.25
        missed = [n for n in self.notes if n[1] > ZONE[1]]
        for n in missed:
            self.notes.remove(n)
            self.lives -= 1
        if self.lives <= 0:
            self.state, self.best = "over", max(self.best, self.score)

    def render(self, scr):
        scr.clear()
        for k in range(25):                             # the keyboard: black keys short, held keys solid
            x = key_x(k)
            if self.keys[k] or k in self.flash:
                scr.rect(x, 29, KEY_W - 1, 3)
            else:
                scr.rect(x, 31, KEY_W - 1, 1)
                if k % 12 in BLACK:
                    scr.rect(x + 1, 29, KEY_W - 3, 1)
        for x in range(0, 128, 4):                      # where notes can be caught
            scr.px(x, int(ZONE[0]) + 1)
        for k, y in self.notes:
            scr.rect(key_x(k), int(y), KEY_W - 1, 3)
        scr.label(1, 1, str(self.score))
        for i in range(self.lives):
            scr.rect(122 - 6 * i, 2, 4, 4)
        if self.state == "over":
            scr.clear()
            scr.text(0, 0, f"CAUGHT {self.score}")
            scr.text(0, 11, f"BEST {self.best}")
            scr.text(0, 24, "PAD 8: PLAY AGAIN")

    @classmethod
    def preview(cls, scr):
        g = cls(seed=5)
        for _ in range(240):                            # an autopilot that catches everything
            keys = [0] * 25
            for k, y in g.notes:
                if ZONE[0] + 2 <= y <= ZONE[1]:
                    keys[k] = 100
            g.step(idle_controls(keys=tuple(keys)), 1 / 30)
        g.render(scr)
