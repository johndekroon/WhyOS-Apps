#!/usr/bin/env python3
"""Flappy on the host: the game played by script (flappy.c, FLAPPY_HOST).

    python3 test_flappy.py [OUTDIR]

With OUTDIR, the screens are saved there as PPM.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
# libwb (wb.h, the fonts): the copy next to it, else why2025-linux's
O = os.path.join(HERE, "libwb")
if not os.path.isfile(os.path.join(O, "wb.h")):
    O = os.path.normpath(os.path.join(HERE, "../../../../why2025-linux/libwb"))

# script -> what the state must say (f flap, kN key code, tN ticks, aN autopilot ticks)
ESC, ENTER, UP, DOWN, HELP, TRIANGLE = 1, 28, 103, 108, 64, 60
CASES = [
    ("", {"mode": "ready", "score": "0"}),
    ("t600", {"mode": "ready"}),                         # waits for the first flap
    ("f t300", {"mode": "dead", "score": "0", "y": "618"}),  # falls to the ground
    ("f a9000", {"mode": "play", "score": "89"}),        # the gaps can be flown through, also
                                                         # long after the hardest level (30)
    ("f t20 k%d t20" % TRIANGLE, {"mode": "play"}),      # a symbol key flaps
    ("f a600 t400", {"mode": "dead", "score": "3"}),     # a pipe ends it
    ("f t300 f", {"mode": "ready"}),                     # a flap after the card: again
    ("f t5 f", {"mode": "play"}),                        # ... but not before it
    ("f t10 k%d t100" % ESC, {"mode": "play paused", "focus": "0"}),
    ("f t10 k%d k%d" % (ESC, ESC), {"mode": "play"}),
    ("f t10 k%d k%d" % (ESC, ENTER), {"mode": "play"}),
    ("f a600 k%d k%d k%d" % (ESC, DOWN, ENTER), {"mode": "ready", "score": "0"}),   # Restart
    ("f t10 k%d k%d k%d k%d" % (ESC, DOWN, DOWN, ENTER), {"mode": "play help"}),  # Keys
    ("f t10 k%d k%d k%d k%d k%d" % (ESC, DOWN, DOWN, ENTER, ESC), {"mode": "play paused"}),
    ("f t10 k%d k%d k%d" % (ESC, UP, ENTER), "closed"),  # Quit
    ("k%d" % HELP, {"mode": "ready help"}),
    ("k%d f k%d" % (HELP, HELP), {"mode": "ready"}),     # Space does nothing in the help
    ("f t10 k%d t100 k%d t5" % (HELP, ESC), {"mode": "play"}),
]


# frames drawn after keys (d), as the badge draws them: the pause and its
# focus, the keys over it and back, resumed, paused again and Restart; the
# game over standing still, a key that changes nothing, the keys, the pause
# and Resume; another game over after a new game
FRAMES = [
    "f t10 d k%d d k%d d k%d d k%d d k%d d k%d d k%d d k%d d k%d d k%d d t3 d k%d d k%d d k%d d"
    % (ESC, DOWN, DOWN, UP, HELP, ESC, DOWN, ENTER, ESC, ESC, ESC, DOWN, ENTER),
    "f t300 d k%d d k%d d k%d d k%d d k%d d k%d d k%d d" % (DOWN, HELP, ESC, ESC, DOWN, UP, ENTER),
    "f t300 d f f t300 d k%d d" % ESC,                   # a second game over, the field moved on
]
SIZE = len(b"P6\n720 720\n255\n") + 720 * 720 * 3


def run(exe, script, ppm=None, env=None, frames=None):
    args = [exe, script] + ([ppm] if ppm else []) + ([frames] if frames else [])
    out = subprocess.run(args, capture_output=True, text=True, check=True, env=env).stdout
    if out.strip() == "closed":
        return "closed"
    return dict(l.split(" ", 1) for l in out.splitlines())


def main():
    tmp = tempfile.mkdtemp()
    outdir = sys.argv[1] if len(sys.argv) > 1 else None
    exe = os.path.join(tmp, "flappy-host")
    subprocess.run(["cc", "-O1", "-Wall", "-Wextra", "-Werror", "-DFLAPPY_HOST", "-I" + O, "-o", exe,
                    os.path.join(HERE, "flappy.c")], check=True)
    env = dict(os.environ)
    env.pop("APP_CONFIG", None)
    bad = 0
    for i, (script, want) in enumerate(CASES):
        ppm = os.path.join(outdir, "flappy-%02d.ppm" % i) if outdir else None
        got = run(exe, script, ppm, env)
        ok = got == want if want == "closed" else got != "closed" and all(got.get(k) == v for k, v in want.items())
        if not ok:
            bad += 1
            print("FAIL %r: want %r, got %r" % (script, want, got))
    # the best score is kept in APP_CONFIG/best and read at the next start
    env["APP_CONFIG"] = tmp
    first = run(exe, "f a600 t400", None, env)
    again = run(exe, "", None, env)
    lower = run(exe, "f t300", None, env)
    saved = open(os.path.join(tmp, "best")).read().strip()
    if (first.get("best"), again.get("best"), lower.get("best"), saved) != ("3", "3", "3", "3"):
        bad += 1
        print("FAIL best score: %r %r %r, file %r" % (first, again, lower, saved))
    # drawn only where the screen changed (libwb's recording) or whole
    # (WB_FULL=1), frosted or in bands: every frame the same
    for script in FRAMES:
        for bands in (False, True):
            got = {}
            for full in (False, True):
                e = dict(env)
                e.pop("WB_FULL", None)
                e.pop("FLAPPY_BANDS_ONLY", None)
                if full:
                    e["WB_FULL"] = "1"
                if bands:
                    e["FLAPPY_BANDS_ONLY"] = "1"
                path = os.path.join(tmp, "frames-%d.ppm" % full)
                run(exe, script, os.path.join(tmp, "last.ppm"), e, path)
                got[full] = open(path, "rb").read()
            n = script.split().count("d")
            same = len(got[False]) == len(got[True]) == n * SIZE
            if not same:
                print("FAIL %r: %d and %d bytes of frames" % (script, len(got[False]), len(got[True])))
            for i in range(n if same else 0):
                if got[False][i * SIZE:(i + 1) * SIZE] != got[True][i * SIZE:(i + 1) * SIZE]:
                    same = False
                    print("FAIL %r%s: frame %d differs from the one drawn whole" %
                          (script, " in bands" if bands else "", i))
                    break
            if not same:
                bad += 1
    print("%d of %d failed" % (bad, len(CASES) + 1 + 2 * len(FRAMES)))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
