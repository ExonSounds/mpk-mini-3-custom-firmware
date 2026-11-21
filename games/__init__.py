"""The game market's catalog (mpk3_market.py).

Two kinds of entries:
  computer  the game runs on this computer and uses the keyboard as its screen and controller (programs/remote.c);
            a class with TITLE, INFO, CONTROLS, step(controls, dt), render(screen) and preview(screen)
  keyboard  a RAM program (programs/*.bin) that runs on the keyboard itself; to be unloadable from the market it
            answers host message op 2 (QUIT) by returning, like programs/demo.c
Add a game: write the class (games/cave.py is a short example) and list it in CATALOG.
"""
import os
from dataclasses import dataclass
from typing import Callable, Optional

from games.beats import Beats
from games.binterm import BinaryTerminal
from games.cave import Cave
from games.keycatch import KeyCatch
from games.mouse import JoyMouse
from games.race import Race
from games.visualiser import Visualiser

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


@dataclass
class Entry:
    key: str
    title: str
    info: str
    controls: str
    kind: str                                    # "computer" or "keyboard"
    preview: Callable                            # preview(screen): draws a typical moment
    make: Optional[Callable] = None              # computer: returns a new game
    binary: Optional[str] = None                 # keyboard: path of the RAM program


def computer(key, cls):
    return Entry(key, cls.TITLE, cls.INFO, cls.CONTROLS, "computer", cls.preview, make=cls)


def fifths_preview(scr):
    scr.text(0, 0, "RAM PROGRAM")
    for i, h in enumerate((5, 12, 20, 9, 15, 3, 18, 7)):
        scr.rect(72 + 7 * i, 31 - h, 5, h + 1)
    scr.rect(38, 17, 4, 4)
    for k in (0, 4, 7):
        scr.rect(2 * k, 9, 2, 2)


CATALOG = [
    computer("race", Race),
    computer("cave", Cave),
    computer("keycatch", KeyCatch),
    computer("beats", Beats),
    computer("binterm", BinaryTerminal),
    computer("mouse", JoyMouse),
    computer("visualiser", Visualiser),
    Entry("fifths", "Fifths", "A tiny app that runs on the keyboard itself: every key plays with a fifth on top, "
          "pads glow with pressure, joystick and knobs on screen.", "Keys: play (MIDI out)   PROG SELECT: leave",
          "keyboard", fifths_preview, binary=os.path.join(HERE, "programs", "demo.bin")),
]
