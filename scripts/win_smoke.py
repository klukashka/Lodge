"""Runs lodge.exe in a real Windows console (ConPTY) and checks what ends up on the screen.

Usage (on Windows): pip install pywinpty pyte && python scripts/win_smoke.py path\\to\\lodge.exe
"""

import os
import sys
import threading
import time
from typing import Callable

import pyte
from winpty import PtyProcess

ROWS, COLS = 44, 120


def main(exe: str) -> int:
    """
    :param exe: path to lodge.exe, with the ``texts`` folder next to it.
    :return: 0 when every check passed, 1 otherwise.
    """
    env = dict(os.environ, LODGE_SEASON="summer", LODGE_AT="2026-07-01 12:30")
    screen = pyte.Screen(COLS, ROWS)
    stream = pyte.Stream(screen)
    lock = threading.Lock()
    # Started from another folder on purpose: the program has to find its pictures by itself.
    proc = PtyProcess.spawn([os.path.abspath(exe)], cwd=os.environ.get("TEMP", "."), env=env, dimensions=(ROWS, COLS))

    def read() -> None:
        while True:
            try:
                data = proc.read(65536)
            except (EOFError, OSError):
                return
            with lock:
                stream.feed(data)

    threading.Thread(target=read, daemon=True).start()

    def text() -> str:
        with lock:
            return "\n".join(screen.display)

    def wait_for(what: str, check: Callable[[], bool], seconds: float) -> bool:
        end = time.time() + seconds
        while time.time() < end:
            if check():
                print("ok:", what)
                return True
            time.sleep(0.05)
        print("FAILED:", what)
        print(text())
        return False

    # The long underscore line is the roof of the lodge in the hand-drawn picture.
    ok = wait_for("the scene is drawn", lambda: "_" * 40 in text() and "^^^" in text(), 60)
    if ok:
        print(text())
        before = text()
        ok = wait_for("the scene moves", lambda: text() != before, 5)
    if ok:
        proc.write("abc q")
        time.sleep(1)
        ok = wait_for("other keys are ignored", lambda: proc.isalive() and "_" * 40 in text(), 2)
    if ok:
        with lock:
            screen.resize(50, 140)
        proc.setwinsize(50, 140)
        ok = wait_for("the scene follows a resize", lambda: "_" * 40 in text(), 5)
    if ok:
        proc.sendintr()
        ok = wait_for("Ctrl+C shows the farewell", lambda: "Good luck on your journey" in text(), 10)
    if ok:
        ok = wait_for("the program exits", lambda: not proc.isalive(), 20)
    if ok:
        ok = proc.exitstatus == 0
        print("exit status:", proc.exitstatus)
    if proc.isalive():
        proc.terminate(force=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
