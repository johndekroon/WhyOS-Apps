#!/usr/bin/env python3
"""The calculator on the host: sums typed, results checked (calc.c, CALC_HOST).

    python3 sdapps/calc/test_calc.py [--shots DIR]

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

# screens to look at: name, keys typed
SHOTS = [
    ("start", ""),
    ("typing", "12*7+3"),
    ("after-enter", "9.81*72\\n128*3+16"),
    ("operator", "2^10\\n+1-"),
    ("error", "10/0\\n"),
    ("recall", "1+2\\n3*4\\n\\u\\u"),
    ("help", "12\\6"),
    ("log", "1+2\\n3*4\\n0xff\\n\\4"),
    ("settings", "\\5"),
    ("unit-pick", "\\5\\n"),
]

# keys typed -> the tape it leaves ("sum = result" lines), or the error
CASES = [
    ("12*7+3\\n", ["12*7+3 = 87"]),
    ("2^3^2\\n-2^2\\n", ["2^3^2 = 512", "-2^2 = -4"]),
    ("15%*80\\n", ["15%*80 = 12"]),
    ("0xff+1\\n0b101\\n", ["0xff+1 = 256", "0b101 = 5"]),
    ("sin 30\\ncos 90\\ntan 45\\n", ["sin 30 = 0.5", "cos 90 = 0", "tan 45 = 1"]),
    ("\\tsin(pi/2)\\n", ["sin(pi/2) = 1"]),
    ("0.1+0.2\\n1/3\\n", ["0.1+0.2 = 0.3", "1/3 = 0.333333333333333"]),
    ("5!\\n7 mod 3\\n", ["5! = 120", "7 mod 3 = 1"]),
    ("2pi\\n3(4+5)\\n(1+2\\n", ["2pi = 6.28318530717959", "3(4+5) = 27", "(1+2 = 3"]),
    ("3x4\\n0x1f\\n", ["3*4 = 12", "0x1f = 31"]),
    ("6\\n*7\\n+1\\n", ["6 = 6", "ans*7 = 42", "ans+1 = 43"]),
    ("1e20*3\\n2/3*1e-9\\n123456789012345\\n",
     ["1e20*3 = 3e+20", "2/3*1e-9 = 6.66666666666667e-10", "123456789012345 = 123456789012345"]),
    ("1+2\\n3\\n\\u\\u\\n", ["1+2 = 3", "3 = 3", "1+2 = 3"]),
    ("log 1000\\nln e\\nlog2 1024\\nsqrt 16\\n", ["log 1000 = 3", "ln e = 1", "log2 1024 = 10", "sqrt 16 = 4"]),
    ("10/0\\n", "Division by zero"),
    ("foo+1\\n", "Unknown name"),
    ("1+2)\\n", 'A "(" is missing'),
    ("1+\\n", "The sum ends too soon"),
    ("sqrt(-1)\\n", "No square root of a negative number"),
    ("171!\\n", "Too large"),
    ("ans\\n", "No answer yet for ans"),
]


def run(exe, keys):
    out = subprocess.run([exe, keys], capture_output=True, text=True, check=True).stdout
    lines = out.splitlines()
    err = next(l[6:] for l in lines if l.startswith("error "))
    tape = [l for l in lines if " = " in l]
    return err, tape, lines


def main():
    exe = os.path.join(tempfile.mkdtemp(), "calc-host")
    subprocess.run(["cc", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-DCALC_HOST",
                    "-DWB_HOST", "-I" + O, "-I" + WB, "-o", exe, os.path.join(HERE, "calc.c"), "-lm"],
                   check=True)
    bad = 0
    for keys, want in CASES:
        err, tape, lines = run(exe, keys)
        ok = (err == want) if isinstance(want, str) else (not err and tape == want)
        if not ok:
            bad += 1
            print("FAIL %r: want %r, got error %r tape %r" % (keys, want, err, tape))
    _, _, lines = run(exe, "abc\\e\\e")
    if lines[0] != "closed":
        bad += 1
        print("FAIL: Esc on an empty line does not close")
    # the symbol keys (Docs/06-design-system.md 6.6): circle the log, cloud
    # the settings, diamond the help; Esc goes back one layer, never closes
    # from there
    keys = [
        ("\\4", "screen log", "unit deg"),
        ("\\4\\e", "screen calc", "unit deg"),
        ("\\5\\n\\d\\n", "screen settings", "unit rad"),
        ("\\5\\n\\d\\e", "screen settings", "unit deg"),
        ("\\5\\n\\d\\n\\e", "screen calc", "unit rad"),
        ("12\\6", "screen calc help", "unit deg"),
        ("12\\6\\e", "screen calc", "unit deg"),
        ("\\6\\6\\1\\2\\3", "screen calc", "unit deg"),
    ]
    for k, screen, unit in keys:
        _, _, lines = run(exe, k)
        if lines[:2] != [screen, unit]:
            bad += 1
            print("FAIL %r: want %r, %r; got %r" % (k, screen, unit, lines[:2]))
    _, _, lines = run(exe, "12\\6\\e")
    if "line 12" not in lines:
        bad += 1
        print("FAIL: closing the help cleared the line")
    # the bands drawn again after each key give the screen drawn whole
    for _, k in SHOTS + [("", "12\\b\\b(1+2)/3\\n\\t\\l\\r\\e\\e")]:
        _, _, lines = run(exe, k)
        if "stale" in lines:
            bad += 1
            print("FAIL %r: drawn key by key, the screen differs from one drawn whole" % k)
    print("%d of %d failed" % (bad, len(CASES) + 2 + len(keys) + len(SHOTS) + 1))
    if "--shots" in sys.argv:
        shots(exe, sys.argv[sys.argv.index("--shots") + 1])
    sys.exit(1 if bad else 0)


def ppm_to_png(ppm, png):
    """A P6 PPM as a PNG (no Pillow needed)."""
    with open(ppm, "rb") as f:
        data = f.read()
    head = data.split(b"\n", 3)
    w, h = map(int, head[1].split())
    px = head[3]
    raw = b"".join(b"\0" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    with open(png, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def shots(exe, out):
    os.makedirs(out, exist_ok=True)
    for name, keys in SHOTS:
        ppm = os.path.join(out, name + ".ppm")
        subprocess.run([exe, keys, ppm], capture_output=True, check=True)
        ppm_to_png(ppm, os.path.join(out, name + ".png"))
        os.remove(ppm)
        print("drew", os.path.join(out, name + ".png"))


if __name__ == "__main__":
    main()
