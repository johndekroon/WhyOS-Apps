#!/usr/bin/env python3
"""Marble Orbit on the host: the physics, the elements, lives, scores and the
screens, played by script (marble.c, MARBLE_HOST), and every level checked
(levels.h).

    python3 test_marble.py [OUTDIR]

With OUTDIR, the screens are saved there as PPM.
"""
import os
import re
import subprocess
import sys
import tempfile
from collections import deque

HERE = os.path.dirname(os.path.abspath(__file__))
O = HERE                 # wbdraw.h and the fonts are next to it
ESC, ENTER, UP, DOWN, LEFT, RIGHT = 1, 28, 103, 108, 105, 106
SQUARE, TRIANGLE, CROSS, CIRCLE, CLOUD, DIAMOND = 59, 60, 61, 62, 63, 64
G1 = 4096                       # a g, in sensor counts

bad = checks = 0


def check(what, ok, detail=""):
    global bad, checks
    checks += 1
    if not ok:
        bad += 1
        print("FAIL", what, detail)


def level_maps():
    src = open(os.path.join(HERE, "levels.h")).read()
    out = []
    for name, rows in re.findall(r'\{ "([^"]+)", "[^"]*", \d+, \d+, \d+,\s*((?:"[^"]*"\s*)+)\}', src):
        out.append((name, re.findall(r'"([^"]*)"', rows)))
    return out


def mp(*rows):
    """A test map: rows inside the border, from the top; the rest floor."""
    inner = [r.ljust(16, ".") for r in rows] + ["." * 16] * (16 - len(rows))
    return "#" * 18 + "".join("#" + r + "#" for r in inner) + "#" * 18


class Game:
    def __init__(self, exe, shots):
        self.exe, self.shots = exe, shots
        self.dir = tempfile.mkdtemp()

    def run(self, script, map_=None, shot=None, laser=0):
        env = dict(os.environ, APP_CONFIG=self.dir, MARBLE_LASER=str(laser))
        env.pop("MARBLE_MAP", None)
        if map_:
            env["MARBLE_MAP"] = map_
        ppm = os.path.join(self.shots, shot + ".ppm") if shot and self.shots else ""
        out = subprocess.run([self.exe, script, ppm], capture_output=True, text=True, env=env,
                             check=True).stdout
        st = {"closed": "closed" in out}
        for line in out.splitlines():
            k, _, v = line.partition(" ")
            if not v:
                continue
            st[k] = int(v) if re.fullmatch(r"-?\d+", v) else v
        return st

    def file(self, name):
        p = os.path.join(self.dir, name)
        return open(p).read() if os.path.exists(p) else ""


def check_levels():
    """Every level: the format, and a way from S to G (ramps jump holes,
    gates after a button that can be reached, portals in pairs)."""
    dirs = {'^': (0, -1), 'v': (0, 1), '<': (-1, 0), '>': (1, 0)}
    maps = level_maps()
    check("18 levels", len(maps) == 18, len(maps))
    for name, m in maps:
        ok = len(m) == 18 and all(len(r) == 18 for r in m)
        check(name + ": 18 x 18", ok)
        if not ok:
            continue
        flat = "".join(m)
        check(name + ": walls around", all(c == '#' for c in m[0] + m[17]) and
              all(r[0] == '#' and r[17] == '#' for r in m))
        check(name + ": one start, one goal", flat.count('S') == 1 and flat.count('G') == 1)
        check(name + ": 3 stars at most", flat.count('*') <= 3)
        check(name + ": portals in pairs", all(flat.count(a) == flat.count(b) <= 1 for a, b in ("Tt", "Yy", "Zz")))
        check(name + ": known tiles", set(flat) <= set(".#OSG*XsicwURDLurdlkgbqBMH=~TtYyZz^v<>"),
              set(flat) - set(".#OSG*XsicwURDLurdlkgbqBMH=~TtYyZz^v<>"))

        def at(x, y):
            return m[y][x] if 0 <= x < 18 and 0 <= y < 18 else '#'
        portal = {m[y][x]: (x, y) for y in range(18) for x in range(18) if m[y][x] in "TtYyZz"}

        def reach(gates):
            s = flat.index('S')
            q = deque([(s % 18, s // 18)])
            seen = set(q)
            while q:
                x, y = q.popleft()
                c = at(x, y)
                nxt = [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]
                if c in dirs:
                    dx, dy = dirs[c]
                    for k in range(1, 7):
                        t = at(x + dx * k, y + dy * k)
                        if t in "#w":
                            break
                        if t not in "OH":
                            nxt.append((x + dx * k, y + dy * k))
                if c in portal and c.swapcase() in portal:
                    nxt.append(portal[c.swapcase()])
                for p in nxt:
                    t = at(*p)
                    if p not in seen and t not in "#wOH" and (t != 'g' or gates):
                        seen.add(p)
                        q.append(p)
            return seen
        closed = reach(False)
        seen = reach(any(at(*p) == 'k' for p in closed))
        g = flat.index('G')
        check(name + ": the goal can be reached", (g % 18, g // 18) in seen)
        check(name + ": every star can be reached",
              all((i % 18, i // 18) in seen for i, c in enumerate(flat) if c == '*'))
        if 'g' in flat:
            check(name + ": the gates matter", (g % 18, g // 18) not in closed)


def main():
    shots = sys.argv[1] if len(sys.argv) > 1 else ""
    if shots:
        os.makedirs(shots, exist_ok=True)
    exe = os.path.join(tempfile.mkdtemp(), "marble-host")
    subprocess.run(["cc", "-O1", "-Wall", "-Wextra", "-Werror", "-DMARBLE_HOST", "-I" + O, "-o", exe,
                    os.path.join(HERE, "marble.c")], check=True)
    check_levels()
    g = Game(exe, shots)

    # rolling: a tilt to the right moves it right, not down; it speeds up
    open_ = mp("S")
    s = g.run("l0 x2000 t30", open_)
    s2 = g.run("l0 x2000 t60", open_)
    check("rolls right", s["x"] > 60 and abs(s["y"] - 60) <= 1, s)
    check("speeds up", s2["vx"] > s["vx"] > 0, (s["vx"], s2["vx"]))
    s = g.run("l0 y2000 t30", open_)
    check("rolls down", s["y"] > 60 and abs(s["x"] - 60) <= 1, s)
    s = g.run("l0 x30 t60", open_)
    check("a tilt under the dead zone: still", s["x"] == 60, s)

    # walls: tilted hard against one, it never gets through
    wall = mp("S........#")
    s = g.run("l0 x8000 t400", wall)
    check("stops at a wall", s["x"] <= 400 - 14 and s["x"] > 370, s)
    s = g.run("l0 x8000 y8000 t600", mp("S"))
    check("held in the corner, still", abs(s["x"] - 666) <= 1 and abs(s["y"] - 666) <= 1 and
          s["vx"] == s["vy"] == 0, s)

    # surfaces: the same push, then let go: ice slides furthest, sand least
    far = {}
    for name, c in (("floor", "."), ("sand", "s"), ("ice", "i")):
        s = g.run("l0 x3000 t25 x0 t120", mp("S" + c * 15, c * 16))
        far[name] = s["x"]
    check("ice > floor > sand", far["ice"] > far["floor"] > far["sand"], far)

    # holes: a life each; three and the level starts over
    hole = mp("S..O")
    s = g.run("l0 x2000 t60", hole)
    check("falls in a hole", s["mode"] == "fall", s)
    s = g.run("l0 x2000 t45 x0 t60", hole)
    check("a life less, back at the start", s["lives"] == 2 and s["x"] == 60 and s["mode"] == "play", s)
    s = g.run("l0 x2000 t100 t100 t50", hole)
    check("out of lives", s["mode"] == "over", s)
    s = g.run("l0 x2000 t100 t100 t50 x0 t200", hole)
    check("the level starts over", s["mode"] == "ready" and s["lives"] == 3 and s["time"] == 0, s)
    s = g.run("l0 x2000 t100 t100 t50 k%d" % ENTER, hole)
    check("Enter: at once", s["mode"] == "ready", s)
    s = g.run("l0 x2000 t45 x0 t60", mp("SX.O"))
    check("a flag: back there", s["lives"] == 2 and s["x"] == 100, s)

    # ramps: fast enough it jumps the gap, too slow it falls in
    ramp = mp("S....>OO.......", "................")
    s = g.run("l0 x2500 t40", ramp)
    jumped = s["air"] > 0
    s = g.run("l0 x2500 t80 x0 t60", ramp)
    check("a ramp: jumps", jumped or s["x"] > 320, s)
    check("lands past the gap", s["x"] > 320 and s["lives"] == 3, s)
    s = g.run("l0 x900 t400", mp("S....>OO"))
    check("too slow: no jump", s["lives"] < 3 or s["x"] < 240, s)
    s = g.run("l0 x600 t300", mp("S...>"))
    check("a gentle tilt: the ramp holds it back", s["x"] < 240 and s["air"] == 0, s)

    # water and belts push; a belt harder
    nudge = "l0 x600 t25 x0 t80"
    f = g.run(nudge, mp("S"))["x"]
    w = g.run(nudge, mp("SRRRRRRRRRRRRRR"))["x"]
    b = g.run(nudge, mp("Srrrrrrrrrrrrrr"))["x"]
    check("water pushes", w > f + 20, (f, w))
    check("a belt pushes harder", b > w, (w, b))
    s = g.run(nudge, mp("SLLLLLLLLLLLLLL"))
    check("water the other way", s["x"] < f, (f, s["x"]))

    # a button opens the gates
    gate = mp("S.k", "####g###########")
    s = g.run("l0", gate)
    check("gates shut", s["gates"] == 1)
    s = g.run("l0 x2000 t40", gate)
    check("a button opens them", s["gates"] == 0, s)

    # cracked ice breaks once it is left
    s = g.run("l0 x1500 t40", mp("Scccccc"))
    check("cracked ice breaks behind", s["holes"] >= 1, s)
    s = g.run("l0", mp("Scccccc"))
    check("... not before", s["holes"] == 0)

    # bumpers kick back, magnets pull, black holes swallow
    s = g.run("l0 x2500 t60", mp("S.....B"))
    check("a bumper kicks back", s["vx"] < 0, s)
    s = g.run("l0 t60", mp("S..M"))
    check("a magnet pulls", s["x"] > 70, s)
    s = g.run("l0 t200", mp("S....H"))
    check("a black hole swallows", s["mode"] in ("fall", "over") or s["lives"] < 3, s)

    # portals: in at one, out at the other, as fast
    s = g.run("l0 x2500 t60", mp("S..T", "", "", "", "", "", "", "", "", "", "t"))
    check("through a portal, as fast", s["y"] > 400 and s["vx"] > 0, s)

    # lasers: on for the first period, then off
    s = g.run("l0 x3000 t50", mp("S..="), laser=90)
    check("a laser burns", s["lives"] < 3 or s["mode"] == "fall", s)
    s = g.run("l0 x3000 t50", mp("S..~"), laser=90)
    check("an off laser lets it by", s["mode"] == "play" and s["x"] > 200, s)
    s = g.run("l0 t100 x3000 t50", mp("S..~"), laser=90)
    check("... until it comes on", s["mode"] != "play" or s["lives"] < 3, s)

    # a sliding stone moves and pushes
    s = g.run("l0 t120", mp("S", "", "b"))
    check("a stone slides", s["mode"] == "play")
    s = g.run("l0 y3000 t200", mp("S", "", "", "", "", "q"))
    check("a stone pushes the marble", s["y"] < 640, s)

    # stars, the goal, the score, kept
    s = g.run("l0 x2000 t120", mp("S.*.......G"))
    check("a star taken", s["stars"] == 1 and s["mode"] == "won", s)
    check("the score: 500 + 250 + 300 + time", s["score"] >= 1050 and s["best"] == s["score"], s)
    check("unlocked the next", s["unlocked"] == 1, s)
    check("kept", "L0 %d" % s["score"] in g.file("scores") and "unlocked 1" in g.file("scores"), g.file("scores"))
    s = g.run("", shot="title")
    check("the high score on the title", s["total"] > 1000 and s["sel"] == 1, s)
    s = g.run("k%d" % RIGHT)
    check("the next level chosen", s["sel"] == 2, s)
    s = g.run("k%d k%d" % (RIGHT, ENTER))
    check("a locked level: not started", s["mode"] == "title")
    s = g.run("k%d" % ENTER)
    check("Enter plays it", s["mode"] == "ready" and s["level"] == 1, s)
    s = g.run("k%d" % ESC)
    check("Esc on the title quits", s["closed"])
    s = g.run("l0 x2000 t200 k%d" % ENTER, mp("S..G"))
    check("Enter after the goal: the next level", s["mode"] == "ready" and s["level"] == 1, s)

    # the card at the start: Enter skips it, and it takes the tilt as level
    s = g.run("k%d" % ENTER, shot="ready")
    check("the level's card", s["mode"] == "ready")
    s = g.run("y2000 k%d k%d t3" % (ENTER, ENTER))
    check("... Enter skips it", s["mode"] == "play", s)
    s = g.run("y2000 k%d k%d t60" % (ENTER, ENTER))
    check("held tilted at the start: that is level", abs(s["y"] - 60) <= 1, s)

    # pause, help, settings, results
    s = g.run("l0 t10 k%d" % ESC, shot="pause")
    check("Esc pauses", s["mode"] == "play pause" and s["focus"] == 0, s)
    s = g.run("l0 x2000 t10 k%d t100" % ESC, open_)
    s2 = g.run("l0 x2000 t10", open_)
    check("... and the marble waits", s["x"] == s2["x"], (s["x"], s2["x"]))
    s = g.run("l0 t10 k%d k%d k%d" % (ESC, DOWN, ENTER))
    check("Restart level", s["mode"] == "ready", s)
    s = g.run("l0 t10 k%d k%d k%d k%d" % (ESC, DOWN, DOWN, ENTER))
    check("Levels", s["mode"] == "title", s)
    s = g.run("l0 t10 k%d k%d k%d k%d k%d" % (ESC, DOWN, DOWN, DOWN, ENTER), shot="help")
    check("Keys", s["mode"] == "play help", s)
    s = g.run("l0 t10 k%d k%d k%d k%d k%d k%d" % (ESC, DOWN, DOWN, DOWN, ENTER, ESC))
    check("... back to the pause", s["mode"] == "play pause" and s["focus"] == 3, s)
    s = g.run("l0 t10 k%d k%d k%d" % (ESC, UP, ENTER))
    check("Quit", s["closed"])
    s = g.run("l0 t10 k%d" % DIAMOND)
    check("the diamond: help", s["mode"] == "play help")
    s = g.run("l0 t10 k%d" % CLOUD, shot="settings")
    check("the cloud: settings", s["mode"] == "play settings")
    s = g.run("k%d k%d" % (CLOUD, ENTER))
    check("controls: arrows", s["control"] == 1 and "control=1" in g.file("settings"), s)
    s = g.run("k%d k%d k%d k%d k%d" % (CLOUD, DOWN, ENTER, DOWN, ENTER))
    check("sensitivity: high", s["sens"] == 2 and "sensitivity=2" in g.file("settings"), s)
    s = g.run("l0 k%d t60 r%d" % (RIGHT, RIGHT), open_)
    check("arrows roll it", s["x"] > 100 and s["vx"] > 0, s)
    s = g.run("k%d k%d" % (CLOUD, ENTER))
    check("controls: tilt again", s["control"] == 0, s)
    s = g.run("k%d" % CIRCLE, shot="results")
    check("the circle: results", s["mode"] == "title results", s)

    # every level loads and draws; one screen per world
    for li in range(18):
        s = g.run("l%d a3 t2" % li, shot="level%d" % li if li % 3 == 1 else None)
        check("level %d plays" % li, s["mode"] == "play" and s["level"] == li, s)

    # every level can be won with this physics: the autopilot steers by tilt
    # alone (a run-up for ramps, a wait for lasers), within the par time
    pars = [int(p) for p in re.findall(r'\{ "[^"]+", "[^"]*", (\d+),', open(os.path.join(HERE, "levels.h")).read())]
    for li in range(18):
        s = g.run("l%d p5400" % li)
        check("level %d won by tilting" % li, s["mode"] == "won" and s["time"] < pars[li] * 60, s)

    print("%d of %d failed" % (bad, checks))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
