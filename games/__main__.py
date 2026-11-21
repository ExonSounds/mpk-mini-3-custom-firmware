"""Play one computer-hosted game from the terminal: python3 -m games race|cave|keycatch|beats"""
import sys

from games import CATALOG
from mpk3_remote import Remote, play

games = {e.key: e for e in CATALOG if e.kind == "computer"}
if len(sys.argv) != 2 or sys.argv[1] not in games:
    sys.exit(f"usage: python3 -m games {'|'.join(games)}   (or the market: python3 mpk3_market.py)")
with Remote() as remote:
    print(f"{games[sys.argv[1]].title} on the keyboard - Ctrl+C or PROG SELECT to stop")
    try:
        play(games[sys.argv[1]].make(), remote)
    except KeyboardInterrupt:
        pass
    except ConnectionError as e:
        print(e)
