#!/usr/bin/env python3
"""The flashlight on the host: keys pressed, modes and screens checked
(flashlight.c, FL_HOST), then its badge code in the sandbox's simulation
(sandbox/test/sbsim.c).

    python3 sdapps/flashlight/test_flashlight.py [--shots DIR]

--shots also draws its screens as PNG into DIR, to look at.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
O = os.path.join(HERE, "..", "..", "patches", "buildroot", "overlay-src")
WB = os.path.join(HERE, "..", "..", "libwb")
SANDBOX = os.path.join(HERE, "..", "..", "sandbox")
sys.path.insert(0, os.path.join(SANDBOX, "test"))

WHITE, RED = (255, 255, 255), (255, 0, 0)

# screens to look at: name, keys pressed
SHOTS = [
    ("start", ""),
    ("start-red", "\\3"),
    ("white", "\\p"),
    ("red", "\\p\\3"),
    ("help", "\\p\\6"),
]

# keys pressed -> mode, keys showing, backlight (-1: the system's)
CASES = [
    ("", 0, 1, -1),
    ("\\p", 0, 0, 80),
    ("\\p\\2", 1, 0, 30),
    ("\\p\\3", 2, 0, 80),
    ("\\p\\3\\1", 0, 0, 80),
    ("\\p\\r", 1, 0, 30),
    ("\\p\\d\\d", 2, 0, 80),
    ("\\p\\r\\r\\r", 0, 0, 80),
    ("\\p\\l", 2, 0, 80),
    ("\\p\\u\\u", 1, 0, 30),
    ("\\p\\4\\5", 0, 0, 80),
    # a mode chosen in the first two seconds: the colour at once, the light after
    ("\\3", 2, 1, -1),
    ("\\3\\p", 2, 0, 80),
]


def run(exe, keys, conf, ppm=None):
    env = dict(os.environ, APP_CONFIG=conf)
    args = [exe, keys] + ([ppm] if ppm else [])
    out = subprocess.run(args, capture_output=True, text=True, check=True, env=env).stdout
    assert "stale" not in out, keys
    vals = dict(l.split() for l in out.splitlines() if " " in l)
    run.help = int(vals["help"])
    return int(vals["mode"]), int(vals["keys"]), int(vals["light"]), "closed" in out


def pixels(path):
    with open(path, "rb") as f:
        data = f.read()
    return data[len(b"P6\n720 720\n255\n"):]


def pixel(data, x, y):
    return tuple(data[(y * 720 + x) * 3:(y * 720 + x) * 3 + 3])


def png(ppm, path):
    data = pixels(ppm)
    raw = b"".join(b"\0" + data[y * 2160:(y + 1) * 2160] for y in range(720))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 720, 720, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def sandboxed(tmp):
    """The badge's code under sbsim: late copies, the policy, sleepd, a virtual clock."""
    import sbsim

    src = os.path.join(HERE, "flashlight.c")
    inc = [O, WB]
    exe = os.path.join(tmp, "flashlight-sim")
    sbsim.build(exe, [src], include=inc)

    def run(name, exe=exe, **kw):
        root = os.path.join(tmp, "sim-" + name)
        sbsim.tree(root, "flashlight")
        return root, os.path.join(root, "out"), sbsim.run(exe, root, os.path.join(root, "out"), "flashlight", **kw)

    # the first two seconds, the modes and the help, with every copy as late as can be
    keys = [(3000, 64), (3600, 1), (4000, 60), (5000, 61), (6000, 1)]
    root, out, r = run("keys", keys=keys, shots=[1000, 2500, 3400, 3800, 4500, 5500], end=20000)
    assert r["status"] == 0 and r["end_ms"] < 7000, r
    assert r["torn"] == 0, r["first_torn"]
    assert r["refused"] == [] and r["ioctls_refused"] == 0, r
    at = [t for t, _ in r["light"]]
    assert [v for _, v in r["light"]] == [80, 30, 80] and 2000 <= at[0] < 2100 and 4000 <= at[1] < 4300, r["light"]
    start, plain, help_, closed = (sbsim.pixels(out, t) for t in (1000, 2500, 3400, 3800))
    assert sbsim.pixel(start, 360, 690) != WHITE and sbsim.pixel(start, 360, 300) == WHITE
    assert plain == bytes(WHITE) * (720 * 720) and closed == plain
    # the help whole: every band dimmed, not only the last
    assert all(sbsim.pixel(help_, 20, y) != WHITE for y in range(4, 720, 40))
    assert sbsim.pixels(out, 5500) == bytes(RED) * (720 * 720)
    with open(os.path.join(root, "root/.whyos/config/flashlight/mode")) as f:
        assert f.read() == "2"

    # ten minutes without a key: the screen stays on
    _, _, r = run("awake", end=600000)
    assert r["slept"] == 0 and r["touches"] > 100 and r["refused"] == [], r

    # standby from the user: no touch wakes the badge, no light on a blank screen
    _, _, r = run("standby", SBSIM_STANDBY="10000-20000", end=30000)
    assert r["woken"] == 0 and r["touches_in_standby"] == 0 and r["lights_in_standby"] == 0, r
    # ... also when it starts in the first two seconds: the light comes after it
    _, _, r = run("standby-early", SBSIM_STANDBY="500-5000", end=8000)
    assert r["lights_in_standby"] == 0 and r["light"] and r["light"][0][0] >= 5000, r

    # the logo key: SIGTERM from apprun
    _, _, r = run("term", SBSIM_TERM=3000, end=20000)
    assert r["status"] == 0 and r["end_ms"] < 4000, r

    # and the two faults of 1.0.0 still show here, or this proves nothing
    no_wait = os.path.join(tmp, "libwb-no-wait")
    shutil.copytree(WB, no_wait)
    with open(os.path.join(no_wait, "wbui.h")) as f:
        text = f.read()
    assert "(void)getppid();" in text
    with open(os.path.join(no_wait, "wbui.h"), "w") as f:
        f.write(text.replace("(void)getppid();", ""))
    bad = os.path.join(tmp, "flashlight-no-wait")
    sbsim.build(bad, [src], include=[O, no_wait])
    _, _, r = run("no-wait", exe=bad, keys=keys, end=20000)
    assert r["torn"] > 0, "a torn frame went unseen"

    with open(os.path.join(SANDBOX, "policy.h")) as f:
        policy = f.read()
    assert '"/run/active", ' in policy and '"/run/standby", ' in policy
    old = os.path.join(tmp, "old-policy")
    os.makedirs(old)
    with open(os.path.join(old, "policy.h"), "w") as f:
        f.write(policy.replace('"/run/active", ', "").replace('"/run/standby", ', ""))
    bad = os.path.join(tmp, "flashlight-old-policy")
    sbsim.build(bad, [src], flags=["-I" + old], include=inc)
    _, _, r = run("old-policy", exe=bad, end=600000)
    assert r["slept"] > 0 and ("write", "/run/active") in r["refused"], "a refused touch went unseen"
    print("sandbox: ok")


def main():
    tmp = tempfile.mkdtemp()
    exe = os.path.join(tmp, "flashlight-host")
    subprocess.run(["cc", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-result",
                    "-DFL_HOST", "-I" + O, "-I" + WB, os.path.join(HERE, "flashlight.c"), "-o", exe], check=True)

    for keys, mode, showing, light in CASES:
        conf = tempfile.mkdtemp(dir=tmp)
        got = run(exe, keys, conf)[:3]
        assert got == (mode, showing, light), (keys, got)

    # Esc closes, Fn+Esc is sleepd's
    conf = tempfile.mkdtemp(dir=tmp)
    assert run(exe, "\\p\\e", conf)[3]
    assert not run(exe, "\\p\\f\\e\\g", conf)[3]

    # the diamond opens the help; Esc and the diamond close it, other keys do nothing there
    conf = tempfile.mkdtemp(dir=tmp)
    ppm = os.path.join(tmp, "h.ppm")
    assert run(exe, "\\p\\6", conf, ppm)[:3] == (0, 0, 80) and run.help == 1
    assert pixel(pixels(ppm), 360, 360) != WHITE
    assert run(exe, "\\p\\6\\3\\r", conf)[:3] == (0, 0, 80) and run.help == 1
    got = run(exe, "\\p\\6\\e", conf, ppm)
    assert not got[3] and run.help == 0 and pixels(ppm) == bytes(WHITE) * (720 * 720)
    run(exe, "\\p\\6\\6", conf, ppm)
    assert run.help == 0 and pixels(ppm) == bytes(WHITE) * (720 * 720)
    assert not run(exe, "\\p\\6\\f\\e\\g", conf)[3] and run.help == 1
    # opened in the first two seconds: the light still comes, the hint bar is gone after
    assert run(exe, "\\6\\p\\e", conf, ppm)[:3] == (0, 0, 80)
    assert pixels(ppm) == bytes(WHITE) * (720 * 720)
    assert run(exe, "\\6\\e", conf)[:3] == (0, 1, -1)

    # the last mode comes back, and starts at the system's brightness again
    conf = tempfile.mkdtemp(dir=tmp)
    run(exe, "\\p\\3", conf)
    assert run(exe, "", conf)[:3] == (2, 1, -1)
    assert run(exe, "\\p", conf)[:3] == (2, 0, 80)
    run(exe, "\\u", conf)
    assert run(exe, "", conf)[0] == 1

    # a file that makes no sense is the first mode
    conf = tempfile.mkdtemp(dir=tmp)
    with open(os.path.join(conf, "mode"), "w") as f:
        f.write("x")
    assert run(exe, "", conf)[0] == 0

    # without a place to keep it, it still works
    assert run(exe, "\\p\\2", os.path.join(tmp, "missing"))[:3] == (1, 0, 30)

    # the keys show for the first two seconds, then the screen is one colour
    conf = tempfile.mkdtemp(dir=tmp)
    ppm = os.path.join(tmp, "s.ppm")
    run(exe, "", conf, ppm)
    d = pixels(ppm)
    assert pixel(d, 10, 10) == WHITE and pixel(d, 360, 690) != WHITE
    run(exe, "\\p", conf, ppm)
    assert pixels(ppm) == bytes(WHITE) * (720 * 720)
    run(exe, "\\p\\3", conf, ppm)
    assert pixels(ppm) == bytes(RED) * (720 * 720)
    assert not os.path.exists(os.path.join(conf, "mode.new"))

    if "--shots" in sys.argv:
        out = sys.argv[sys.argv.index("--shots") + 1]
        os.makedirs(out, exist_ok=True)
        for name, keys in SHOTS:
            ppm = os.path.join(tmp, name + ".ppm")
            run(exe, keys, tempfile.mkdtemp(dir=tmp), ppm)
            png(ppm, os.path.join(out, name + ".png"))
    print("ok: %d cases" % len(CASES))
    if os.path.exists(SANDBOX):		# not next to a copy of this in WhyOS-Apps
        sandboxed(tmp)


if __name__ == "__main__":
    main()
