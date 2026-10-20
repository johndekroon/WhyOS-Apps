#!/usr/bin/env python3
"""Snippets on the host: editing, storage and the keyboard reports
(snippets.c, SN_HOST).

    python3 sdapps/snippets/test_snippets.py [screenshots dir]
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
O = os.path.join(HERE, "..", "..", "patches", "buildroot", "overlay-src")

# HID usage -> (character, shifted character) on a US keyboard
US = {0x28: ("\n", "\n"), 0x2b: ("\t", "\t"), 0x2c: (" ", " ")}
for i, (lo, hi) in enumerate(zip("abcdefghijklmnopqrstuvwxyz", "ABCDEFGHIJKLMNOPQRSTUVWXYZ")):
    US[0x04 + i] = (lo, hi)
for i, (lo, hi) in enumerate(zip("1234567890", "!@#$%^&*()")):
    US[0x1e + i] = (lo, hi)
for u, lo, hi in [(0x2d, "-", "_"), (0x2e, "=", "+"), (0x2f, "[", "{"), (0x30, "]", "}"),
                  (0x31, "\\", "|"), (0x33, ";", ":"), (0x34, "'", '"'), (0x35, "`", "~"),
                  (0x36, ",", "<"), (0x37, ".", ">"), (0x38, "/", "?")]:
    US[u] = (lo, hi)


def presses(hid):
    """The reports as (modifiers, key) for each key that went down."""
    out, prev = [], 0
    for line in open(hid).read().split():
        mods, key = int(line[0:2], 16), int(line[4:6], 16)
        if key and key != prev:
            out.append((mods, key))
        prev = key
    return out


def us_text(ps):
    return "".join(US[k][1 if m & 0x22 else 0] for m, k in ps)


class App:
    def __init__(self, exe, shots):
        self.exe, self.shots = exe, shots
        self.dir = tempfile.mkdtemp()
        self.hid = os.path.join(self.dir, "hid")

    def run(self, keys, computer=True, shot=None):
        env = dict(os.environ, APP_CONFIG=self.dir, SN_HID=self.hid, SN_COMPUTER="1" if computer else "0")
        if os.path.exists(self.hid):
            os.unlink(self.hid)
        ppm = os.path.join(self.shots, shot + ".ppm") if shot and self.shots else ""
        out = subprocess.run([self.exe, keys, ppm], capture_output=True, text=True, env=env,
                             check=True).stdout
        st = {"snips": []}
        for line in out.splitlines():
            k, _, v = line.partition(" ")
            if k == "snip":
                st["snips"].append(v)
            else:
                st[k] = v
        return st

    def presses(self):
        return presses(self.hid) if os.path.exists(self.hid) else []

    def file(self, name, text=None):
        p = os.path.join(self.dir, name)
        if text is not None:
            open(p, "w").write(text)
        return open(p).read()


bad = 0
checks = 0


def check(what, got, want):
    global bad, checks
    checks += 1
    if got != want:
        bad += 1
        print("FAIL %s: want %r, got %r" % (what, want, got))


def main():
    shots = sys.argv[1] if len(sys.argv) > 1 else ""
    if shots:
        os.makedirs(shots, exist_ok=True)
    exe = os.path.join(tempfile.mkdtemp(), "snippets-host")
    subprocess.run(["cc", "-O1", "-Wall", "-Wextra", "-Werror", "-DSN_HOST", "-I" + O, "-o", exe,
                    os.path.join(HERE, "snippets.c")], check=True)

    # adding: the triangle, a name, Tab, a text with a new line and a tab
    a = App(exe, shots)
    s = a.run("", shot="empty")
    check("empty list", s["snips"], [])
    s = a.run("\\2Mail\\tjohn@example.org\\n", shot="list")
    check("added", s["snips"], ["Mail|john@example.org"])
    check("saved notice", s["notice"], "Saved")
    s = a.run("\\2Login\\tuser\\tpass\\\\word\\N\\n")
    check("tab, backslash, new line", s["snips"][1], "Login|user\\tpass\\\\word\\n")
    check("file", a.file("snippets.txt").splitlines()[2:],
          ["Mail\tjohn@example.org", "Login\tuser\\tpass\\\\word\\n"])

    # typing: Enter on the focused one, reports read back as US text
    s = a.run("\\d\\n")
    check("typed notice", s["notice"], "Typed 15 characters")
    check("typed text", us_text(a.presses()), "user\tpass\\word\n")
    s = a.run("\\n")
    check("typed first", us_text(a.presses()), "john@example.org")
    every = "".join(chr(c) for c in range(32, 127))
    a.file("snippets.txt", "#c\r\n\t" + every.replace("\\", "\\\\") + "\r\n")
    s = a.run("\\n")
    check("loaded with CRLF", len(s["snips"]), 1)
    check("all of ASCII", us_text(a.presses()), every)
    s = a.run("\\n", computer=False, shot="nocomputer")
    check("no computer", s["notice"], "No computer: connect one to the bottom USB-C port (J8)")
    check("no computer, no keys", a.presses(), [])

    # layouts: accents through the file; settings via the cloud key
    a.file("snippets.txt", "x\téëç€'a\n")
    s = a.run("\\n")
    check("US skips", s["notice"], "Typed 2 characters; skipped 4 the layout cannot type")
    check("US keys", us_text(a.presses()), "'a")
    s = a.run("\\5\\n\\d\\n\\e", shot="settings")
    check("layout saved", a.file("settings"), "layout=1\nslow=0\n")
    s = a.run("\\n")
    check("intl notice", s["notice"], "Typed 6 characters")
    check("intl keys", a.presses(), [
        (0, 0x34), (0, 0x08),			# ' e
        (0x02, 0x34), (0, 0x08),		# " e
        (0, 0x34), (0, 0x06),			# ' c
        (0x40, 0x22),				# AltGr 5
        (0, 0x34), (0, 0x2c), (0, 0x04),	# ' space a
    ])
    s = a.run("\\5\\n\\d\\d\\n\\e")
    check("mac saved", a.file("settings"), "layout=2\nslow=0\n")
    a.run("\\n")
    check("mac keys", a.presses(), [
        (0x04, 0x08), (0, 0x08),		# Option-e e
        (0x04, 0x18), (0, 0x08),		# Option-u e
        (0x04, 0x06),				# Option-c
        (0x06, 0x1f),				# Option-Shift-2
        (0, 0x34), (0, 0x04),			# ' a
    ])
    s = a.run("\\5\\d\\n\\d\\n\\e")
    check("slow saved", a.file("settings"), "layout=2\nslow=1\n")

    # the editor
    b = App(exe, shots)
    s = b.run("\\2\\n")
    check("empty text refused", (s["screen"], s["error"]), ("edit", "Type a text first"))
    s = b.run("\\2ab\\tline1\\Nline2\\u\\f\\lX\\n")
    check("up, Fn+left", s["snips"], ["ab|Xline1\\nline2"])
    s = b.run("\\3\\b\\b\\e")
    check("Esc cancels", s["snips"], ["ab|Xline1\\nline2"])
    s = b.run("\\3\\u\\u\\b\\n")
    check("up into the name", s["snips"], ["a|Xline1\\nline2"])
    s = b.run("\\3\\f\\l\\f\\b\\n")
    check("Fn+Left, Fn+Backspace", s["snips"], ["a|Xline1\\nine2"])
    b.file("snippets.txt", "n\tabé\n")
    s = b.run("\\3\\b\\n")
    check("Backspace takes a whole character", s["snips"], ["n|ab"])
    s = b.run("\\2" + "x" * 45 + "\\t" + "y" * 1010 + "\\n", shot="full")
    check("limits", s["snips"][1], "x" * 40 + "|" + "y" * 1000)
    s = b.run("\\2\\tlong " + "word " * 60, shot="edit")
    check("long text in the editor", s["screen"], "edit")

    # the square: move, delete (with Keep first)
    c = App(exe, shots)
    c.run("\\2one\\t1\\n\\2two\\t2\\n\\2three\\t3\\n")
    s = c.run("\\1", shot="menu")
    check("menu", s["screen"], "list menu")
    s = c.run("\\d\\d\\1\\d\\d\\n")	# on three: Type, Edit, Move up
    check("move up", s["snips"], ["one|1", "three|3", "two|2"])
    s = c.run("\\1\\d\\d\\d\\n", shot="delete")	# Type, Edit, Move down, Delete
    check("delete asks", s["screen"], "list delete")
    s = c.run("\\1\\d\\d\\d\\n\\n")
    check("keep", len(s["snips"]), 3)
    s = c.run("\\1\\d\\d\\d\\n\\d\\n")
    check("deleted", s["snips"], ["three|3", "two|2"])
    s = c.run("\\d\\1\\n")
    check("type from the menu", us_text(c.presses()), "2")

    # the right symbol keys: log, settings, help; Esc goes back one layer
    for keys, screen in [("\\4", "log"), ("\\4\\e", "list"), ("\\5", "settings"),
                         ("\\5\\e", "list"), ("\\6", "list help"), ("\\6\\e", "list"),
                         ("\\2\\6", "edit help"), ("\\2\\5\\e", "edit"), ("\\e", "list")]:
        s = c.run(keys, shot="help" if keys == "\\6" else None)
        check("keys %r" % keys, s["screen"], screen)
    s = c.run("abc\\e")
    check("Esc on the list closes", s.get("closed"), "")
    s = c.run("\\n\\4", shot="log")
    check("log", (s["screen"], s["notice"]), ("log", "Typed 1 character"))

    print("%d of %d failed" % (bad, checks))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
