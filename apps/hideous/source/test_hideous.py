#!/usr/bin/env python3
"""HIDeous on the host: editing, storage and the keyboard reports
(hideous.c, SN_HOST).

    python3 sdapps/hideous/test_hideous.py [screenshots dir]
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
O = os.path.join(HERE, "..", "..", "patches", "buildroot", "overlay-src")

# HID usage -> (character, shifted character) on a US keyboard
US = {0x28: ("\n", "\n"), 0x2b: ("\t", "\t"), 0x2c: (" ", " "), 0x29: ("\x1b", "\x1b")}
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
        if len(line) != 16:
            continue
        mods, key = int(line[0:2], 16), int(line[4:6], 16)
        if key and key != prev:
            out.append((mods, key))
        prev = key
    return out


def mouse(hid):
    """The mouse's reports as (buttons, x, y, wheel), signed."""
    sx = lambda b: b - 256 if b > 127 else b  # noqa: E731
    out = []
    for line in open(hid).read().split():
        if len(line) == 8:
            b = bytes.fromhex(line)
            out.append((b[0], sx(b[1]), sx(b[2]), sx(b[3])))
    return out


def us_text(ps):
    return "".join(US[k][1 if m & 0x22 else 0] for m, k in ps)


class App:
    def __init__(self, exe, shots):
        self.exe, self.shots = exe, shots
        self.dir = tempfile.mkdtemp()
        self.hid = os.path.join(self.dir, "hid")

    def run(self, keys, computer=True, shot=None, nomouse=False):
        env = dict(os.environ, APP_CONFIG=self.dir, SN_HID=self.hid, SN_COMPUTER="1" if computer else "0")
        env.pop("SN_NOMOUSE", None)
        if nomouse:
            env["SN_NOMOUSE"] = "1"
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

    def mouse(self):
        return mouse(self.hid) if os.path.exists(self.hid) else []

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
    exe = os.path.join(tempfile.mkdtemp(), "hideous-host")
    subprocess.run(["cc", "-O1", "-Wall", "-Wextra", "-Werror", "-DSN_HOST", "-I" + O, "-o", exe,
                    os.path.join(HERE, "hideous.c")], check=True)

    # adding: the triangle, a name, Tab, a text with a new line and a tab
    a = App(exe, shots)
    s = a.run("", shot="empty")
    check("empty list", s["snips"], [])
    s = a.run("\\2Mail\\tjohn@example.org\\n", shot="list")
    check("added", s["snips"], ["Mail|john@example.org"])
    check("saved notice", s["notice"], "Saved")
    s = a.run("\\2Login\\tuser\\tpass\\\\word\\N\\n")
    check("tab, backslash, new line", s["snips"][1], "Login|user\\tpass\\\\word\\n")
    check("file", [l for l in a.file("snippets.txt").splitlines() if not l.startswith("#")],
          ["Mail\tjohn@example.org", "Login\tuser\\tpass\\\\word\\n"])

    # typing: Enter on the focused one (the first row is the mouse), reports read back as US text
    s = a.run("\\d\\d\\n")
    check("typed notice", s["notice"], "Typed 15 characters")
    check("typed text", us_text(a.presses()), "user\tpass\\word\n")
    s = a.run("\\d\\n")
    check("typed first", us_text(a.presses()), "john@example.org")
    every = "".join(chr(c) for c in range(32, 127))
    a.file("snippets.txt", "#c\r\n\t" + every.replace("\\", "\\\\") + "\r\n")
    s = a.run("\\d\\n")
    check("loaded with CRLF", len(s["snips"]), 1)
    check("all of ASCII", us_text(a.presses()), every)
    # Fast: one report per key, a release only between the same key twice
    a.file("snippets.txt", "x\tHello\n")
    a.run("\\d\\n")
    reports = [l for l in open(a.hid).read().split() if len(l) == 16]
    check("fast reports", [l[:2] + l[4:6] for l in reports],
          ["020b", "0008", "000f", "0000", "000f", "0012", "0000"])
    check("fast text", us_text(a.presses()), "Hello")
    a.file("snippets.txt", "#c\r\n\t" + every.replace("\\", "\\\\") + "\r\n")
    s = a.run("\\d\\n", computer=False, shot="nocomputer")
    check("no computer", s["notice"], "No computer: connect one to the bottom USB-C port (J8)")
    check("no computer, no keys", a.presses(), [])

    # layouts: accents through the file; settings via the cloud key
    a.file("snippets.txt", "x\téëç€'a\n")
    s = a.run("\\d\\n")
    check("US skips", s["notice"], "Typed 2 characters; skipped 4 the layout cannot type")
    check("US keys", us_text(a.presses()), "'a")
    s = a.run("\\5\\n\\d\\n\\e", shot="settings")
    check("layout saved", a.file("settings"), "layout=1\nslow=0\nmouse=1\n")
    s = a.run("\\d\\n")
    check("intl notice", s["notice"], "Typed 6 characters")
    check("intl keys", a.presses(), [
        (0, 0x34), (0, 0x08),			# ' e
        (0x02, 0x34), (0, 0x08),		# " e
        (0, 0x34), (0, 0x06),			# ' c
        (0x40, 0x22),				# AltGr 5
        (0, 0x34), (0, 0x2c), (0, 0x04),	# ' space a
    ])
    s = a.run("\\5\\n\\d\\d\\n\\e")
    check("mac saved", a.file("settings"), "layout=2\nslow=0\nmouse=1\n")
    a.run("\\d\\n")
    check("mac keys", a.presses(), [
        (0x04, 0x08), (0, 0x08),		# Option-e e
        (0x04, 0x18), (0, 0x08),		# Option-u e
        (0x04, 0x06),				# Option-c
        (0x06, 0x1f),				# Option-Shift-2
        (0, 0x34), (0, 0x04),			# ' a
    ])
    s = a.run("\\5\\d\\n\\d\\n\\e")
    check("slow saved", a.file("settings"), "layout=2\nslow=1\nmouse=1\n")
    a.file("snippets.txt", "x\tHi\n")
    a.run("\\d\\n")
    check("slow reports", [l[:2] + l[4:6] for l in open(a.hid).read().split() if len(l) == 16],
          ["0200", "020b", "0200", "0000", "000c", "0000", "0000"])

    # the editor
    b = App(exe, shots)
    s = b.run("\\2\\n")
    check("empty text refused", (s["screen"], s["error"]), ("edit", "Type a text first"))
    s = b.run("\\2ab\\tline1\\Nline2\\u\\f\\lX\\n")
    check("up, Fn+left", s["snips"], ["ab|Xline1\\nline2"])
    s = b.run("\\d\\3\\b\\b\\e")
    check("Esc cancels", s["snips"], ["ab|Xline1\\nline2"])
    s = b.run("\\d\\3\\u\\u\\b\\n")
    check("up into the name", s["snips"], ["a|Xline1\\nline2"])
    s = b.run("\\d\\3\\f\\l\\f\\b\\n")
    check("Fn+Left, Fn+Backspace", s["snips"], ["a|Xline1\\nine2"])
    b.file("snippets.txt", "n\tabé\n")
    s = b.run("\\d\\3\\b\\n")
    check("Backspace takes a whole character", s["snips"], ["n|ab"])
    # keys in a text: Shift+Esc in the editor, \[Esc] in the file, the Esc key on the computer
    s = b.run("\\2Vim\\t\\E:wq\\N\\n", shot="esc")
    check("Shift+Esc in the text", s["snips"][1], "Vim|\\[Esc]:wq\\n")
    check("Esc in the file", b.file("snippets.txt").splitlines()[-1], "Vim\t\\[Esc]:wq\\n")
    s = b.run("\\d\\d\\n")
    check("Esc typed", (s["notice"], us_text(b.presses())), ("Typed 5 characters", "\x1b:wq\n"))
    s = b.run("\\2a\\Eb\\tc\\n")
    check("Shift+Esc in the name: nothing", s["snips"][2], "ab|c")
    # Shift: Backspace, the arrows, the symbol keys as F1-F6; with Fn keyboard mode's layer
    s = b.run("\\2Keys\\tx\\s\\b\\s\\l\\s\\r\\s\\u\\s\\d\\s\\1\\f\\s\\6\\f\\s\\b\\f\\s\\uy\\n", shot="keys")
    check("Shift+keys in the text", s["snips"][3],
          "Keys|x\\[Backspace]\\[Left]\\[Right]\\[Up]\\[Down]\\[F1]\\[F12]\\[Delete]\\[Page Up]y")
    s = b.run("\\d\\d\\d\\d\\n")
    check("keys typed", (s["notice"], b.presses()),
          ("Typed 11 characters", [(0, 0x1b), (0, 0x2a), (0, 0x50), (0, 0x4f), (0, 0x52), (0, 0x51),
                                   (0, 0x3a), (0, 0x45), (0, 0x4c), (0, 0x4b), (0, 0x1c)]))
    # Ctrl and Alt on the badge: the shortcut, its modifiers down first even when Fast
    s = b.run("\\2Copy\\t\\cc\\s\\ct\\a\\t\\n")
    check("Ctrl+C, Ctrl+Shift+T, Alt+Tab", s["snips"][4], "Copy|\\[Ctrl+C]\\[Ctrl+Shift+T]\\[Alt+Tab]")
    b.run("\\d\\d\\d\\d\\d\\n")
    check("shortcut reports", [l[:2] + l[4:6] for l in open(b.hid).read().split() if len(l) == 16][:4],
          ["0100", "0106", "0100", "0000"])
    check("shortcuts typed", b.presses(), [(0x01, 0x06), (0x03, 0x17), (0x04, 0x2b)])
    # the square: Insert key, Win on, then r; a pause; Enter from the list
    s = b.run("\\d\\d\\d\\d\\d\\d\\2Run\\t\\1", shot="keypop")
    check("Insert key opens", s["screen"], "edit keys")
    s = b.run("\\2Run\\t\\1\\d\\d\\d\\nr\\1" + "\\d" * 39 + "\\ncmd\\1" + "\\d" * 7 + "\\n\\1\\e\\n")
    check("Win+R, wait, Enter", s["snips"][5], "Run|\\[Win+R]\\[Wait 0.5s]cmd\\[Enter]")
    s = b.run("\\d" * 6 + "\\n")
    check("typed with the pause", (s["notice"], b.presses()[:2], "wait" in open(b.hid).read()),
          ("Typed 6 characters", [(0x08, 0x15), (0, 0x06)], True))
    s = b.run("\\2Win\\t\\1\\d\\d\\d\\n" + "\\d" * 35 + "\\n\\n")
    check("the modifiers alone", s["snips"][6], "Win|\\[Win]")
    # the file: names in any case, aliases of 2026-10-20, unknown names stay text
    b.file("snippets.txt", "f\t\\[ctrl+alt+delete]\\[Wait 2s]\\[Cmd+Space]\\[Ctrl++]\\[Foo]\\e\\b\n")
    s = b.run("")
    check("names in the file", s["snips"],
          ["f|\\[Ctrl+Alt+Delete]\\[Wait 2.0s]\\[Win+Space]\\[Ctrl+Shift+=]\\\\[Foo]\\[Esc]\\[Backspace]"])
    s = b.run("\\5\\n\\d\\d\\n\\e\\d\\3\\n")
    check("Mac names", b.file("snippets.txt").splitlines()[-1],
          "f\t\\[Ctrl+Option+Delete]\\[Wait 2.0s]\\[Cmd+Space]\\[Ctrl+Shift+=]\\\\[Foo]\\[Esc]\\[Backspace]")
    b.file("snippets.txt", "n\tab\n")
    s = b.run("\\2" + "x" * 45 + "\\t" + "y" * 1010 + "\\n", shot="full")
    check("limits", s["snips"][1], "x" * 40 + "|" + "y" * 1000)
    s = b.run("\\2\\tlong " + "word " * 60, shot="edit")
    check("long text in the editor", s["screen"], "edit")

    # the square: move, delete (with Keep first)
    c = App(exe, shots)
    c.run("\\2one\\t1\\n\\2two\\t2\\n\\2three\\t3\\n")
    s = c.run("\\d\\1", shot="menu")
    check("menu", s["screen"], "list menu")
    s = c.run("\\d\\d\\d\\1\\d\\d\\n")	# on three: Type, Edit, Move up
    check("move up", s["snips"], ["one|1", "three|3", "two|2"])
    s = c.run("\\d\\1\\d\\d\\d\\n", shot="delete")	# Type, Edit, Move down, Delete
    check("delete asks", s["screen"], "list delete")
    s = c.run("\\d\\1\\d\\d\\d\\n\\n")
    check("keep", len(s["snips"]), 3)
    s = c.run("\\d\\1\\d\\d\\d\\n\\d\\n")
    check("deleted", s["snips"], ["three|3", "two|2"])
    s = c.run("\\d\\d\\1\\n")
    check("type from the menu", us_text(c.presses()), "2")

    # the right symbol keys: log, settings, help; Esc goes back one layer
    for keys, screen in [("\\4", "log"), ("\\4\\e", "list"), ("\\5", "settings"),
                         ("\\5\\e", "list"), ("\\6", "list help"), ("\\6\\e", "list"),
                         ("\\2\\6", "edit help"), ("\\2\\5\\e", "edit"), ("\\e", "list")]:
        s = c.run(keys, shot="help" if keys == "\\6" else None)
        check("keys %r" % keys, s["screen"], screen)
    s = c.run("abc\\e")
    check("Esc on the list closes", s.get("closed"), "")
    s = c.run("\\d\\n\\4", shot="log")
    check("log", (s["screen"], s["notice"]), ("log", "Typed 1 character"))

    # the mouse: the list's first row
    d = App(exe, shots)
    s = d.run("\\n", shot="mouse")
    check("mouse screen", (s["screen"], s["notice"]), ("mouse", ""))
    d.run("\\n\\r")
    check("a tap is one pixel", d.mouse(), [(0, 1, 0, 0)])
    d.run("\\n\\H\\l")
    m = d.mouse()
    check("held: left only", {r[0] for r in m} | {r[2] for r in m}, {0})
    check("held: speeds up", (m[0][1], min(r[1] for r in m)), (-1, -12))
    check("held: a second's distance", sum(r[1] for r in m) < -250, True)
    d.run("\\n\\n")
    check("Enter: left button", d.mouse(), [(1, 0, 0, 0), (0, 0, 0, 0)])
    d.run("\\n\\3")
    check("cross: right click", d.mouse(), [(2, 0, 0, 0), (0, 0, 0, 0)])
    d.run("\\n\\f\\u\\f\\d")
    check("Fn+Up/Down: the wheel", d.mouse(), [(0, 0, 0, 1), (0, 0, 0, -1)])
    d.run("\\n\\1\\n")
    check("middle click", d.mouse(), [(4, 0, 0, 0), (0, 0, 0, 0)])
    d.run("\\n\\1\\d\\n")
    check("double click", d.mouse(), [(1, 0, 0, 0), (0, 0, 0, 0)] * 2)
    s = d.run("\\n\\2")
    check("keep awake: a nudge at once", (s["awake"], s["nudges"], d.mouse()),
          ("1", "1", [(0, 1, 0, 0), (0, -1, 0, 0)]))
    s = d.run("\\n\\2\\e\\w\\w", shot="awake")
    check("keep awake goes on in the list", (s["screen"], s["nudges"]), ("list", "3"))
    s = d.run("\\n\\2\\2\\w")
    check("keep awake off", (s["awake"], s["nudges"]), ("0", "1"))
    s = d.run("\\n\\r", computer=False)
    check("no computer", (s["notice"], d.mouse()),
          ("No computer: connect one to the bottom USB-C port (J8)", []))
    s = d.run("\\n\\r", nomouse=True)
    check("no mouse in the image", (s["notice"], d.mouse()),
          ("This WhyOS has no USB mouse yet: update the badge", []))
    s = d.run("\\n\\e")
    check("Esc back to the list", s["screen"], "list")
    s = d.run("\\n\\5\\d\\d\\n\\d\\n\\e\\H\\r")
    check("mouse speed Fast", (d.file("settings")[-8:], s["screen"], max(r[1] for r in d.mouse())),
          ("mouse=2\n", "mouse", 24))
    s = d.run("\\n\\6")
    check("mouse help", s["screen"], "mouse help")

    print("%d of %d failed" % (bad, checks))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
