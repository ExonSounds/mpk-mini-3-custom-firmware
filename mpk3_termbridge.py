"""Runs a shell in this terminal window and also types whatever arrives on a Unix socket into it.

Started by the Binary Typer app (games/binterm.py) inside a new terminal window; you can type in the window as
usual too. When the app is unloaded the shell keeps running here; it ends when you exit it or close the window.
usage: python3 mpk3_termbridge.py SOCKET [COMMAND ...]     (COMMAND defaults to $SHELL)
"""
import fcntl
import os
import pty
import select
import signal
import socket
import sys
import termios
import tty


def main(argv):
    if not argv:
        sys.exit(__doc__)
    cmd = argv[1:] or [os.environ.get("SHELL", "/bin/sh")]
    pid, master = pty.fork()
    if pid == 0:
        os.execvp(cmd[0], cmd)
    sock = socket.socket(socket.AF_UNIX)
    try:
        sock.connect(argv[0])
    except OSError:
        sock = None  # no app listening: just a terminal
    tty_in = os.isatty(0)

    def resize(*_):
        if tty_in:
            fcntl.ioctl(master, termios.TIOCSWINSZ, fcntl.ioctl(0, termios.TIOCGWINSZ, b"\0" * 8))

    resize()
    signal.signal(signal.SIGWINCH, resize)
    saved = termios.tcgetattr(0) if tty_in else None
    if tty_in:
        tty.setraw(0)
    try:
        while True:
            fds = [0, master] + ([sock] if sock else [])
            try:
                ready = select.select(fds, [], [])[0]
            except InterruptedError:  # SIGWINCH
                continue
            if master in ready:
                try:
                    data = os.read(master, 4096)
                except OSError:  # the shell exited
                    break
                if not data:
                    break
                os.write(1, data)
            if 0 in ready:
                data = os.read(0, 4096)
                if not data:
                    break
                os.write(master, data)
            if sock and sock in ready:
                data = sock.recv(4096)
                if data:
                    os.write(master, data)
                else:
                    sock.close()
                    sock = None  # the app was unloaded; the shell stays
    finally:
        if saved:
            termios.tcsetattr(0, termios.TCSADRAIN, saved)
        os.waitpid(pid, 0)


if __name__ == "__main__":
    main(sys.argv[1:])
