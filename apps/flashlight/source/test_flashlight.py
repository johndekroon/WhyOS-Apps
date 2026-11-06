#!/usr/bin/env python3
"""The flashlight on the host: keys pressed, modes and screens checked
(flashlight.c, FL_HOST).

    python3 sdapps/flashlight/test_flashlight.py [--shots DIR]

--shots also draws its screens as PNG into DIR, to look at.
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
O = os.path.join(HERE, "..", "..", "patches", "buildroot", "overlay-src")
WB = os.path.join(HERE, "..", "..", "libwb")

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
    ("\\p", 0, 0, 255),
    ("\\p\\2", 1, 0, 127),
    ("\\p\\3", 2, 0, 255),
    ("\\p\\3\\1", 0, 0, 255),
    ("\\p\\r", 1, 0, 127),
    ("\\p\\d\\d", 2, 0, 255),
    ("\\p\\r\\r\\r", 0, 0, 255),
    ("\\p\\l", 2, 0, 255),
    ("\\p\\u\\u", 1, 0, 127),
    ("\\p\\4\\5", 0, 0, 255),
    # a mode chosen in the first two seconds: the colour at once, the light after
    ("\\3", 2, 1, -1),
    ("\\3\\p", 2, 0, 255),
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
    assert run(exe, "\\p\\6", conf, ppm)[:3] == (0, 0, 255) and run.help == 1
    assert pixel(pixels(ppm), 360, 360) != WHITE
    assert run(exe, "\\p\\6\\3\\r", conf)[:3] == (0, 0, 255) and run.help == 1
    got = run(exe, "\\p\\6\\e", conf, ppm)
    assert not got[3] and run.help == 0 and pixels(ppm) == bytes(WHITE) * (720 * 720)
    run(exe, "\\p\\6\\6", conf, ppm)
    assert run.help == 0 and pixels(ppm) == bytes(WHITE) * (720 * 720)
    assert not run(exe, "\\p\\6\\f\\e\\g", conf)[3] and run.help == 1
    # opened in the first two seconds: the light still comes, the keys' panel is gone after
    assert run(exe, "\\6\\p\\e", conf, ppm)[:3] == (0, 0, 255)
    assert pixels(ppm) == bytes(WHITE) * (720 * 720)
    assert run(exe, "\\6\\e", conf)[:3] == (0, 1, -1)

    # the last mode comes back, and starts at the system's brightness again
    conf = tempfile.mkdtemp(dir=tmp)
    run(exe, "\\p\\3", conf)
    assert run(exe, "", conf)[:3] == (2, 1, -1)
    assert run(exe, "\\p", conf)[:3] == (2, 0, 255)
    run(exe, "\\u", conf)
    assert run(exe, "", conf)[0] == 1

    # a file that makes no sense is the first mode
    conf = tempfile.mkdtemp(dir=tmp)
    with open(os.path.join(conf, "mode"), "w") as f:
        f.write("x")
    assert run(exe, "", conf)[0] == 0

    # without a place to keep it, it still works
    assert run(exe, "\\p\\2", os.path.join(tmp, "missing"))[:3] == (1, 0, 127)

    # the keys show for the first two seconds, then the screen is one colour
    conf = tempfile.mkdtemp(dir=tmp)
    ppm = os.path.join(tmp, "s.ppm")
    run(exe, "", conf, ppm)
    d = pixels(ppm)
    assert pixel(d, 10, 10) == WHITE and pixel(d, 360, 210) != WHITE
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


if __name__ == "__main__":
    main()
