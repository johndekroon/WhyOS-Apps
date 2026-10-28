#!/usr/bin/env python3
"""BARbarious on the host: editing, storage, the screens and every barcode
type, decoded from what the app draws (barbarious.c and symbol.c with
-DBB_HOST).

    python3 sdapps/barbarious/test_barbarious.py [screenshots dir]

The decoding needs zxing-cpp and PIL (pip install zxing-cpp pillow; in the
build VM: ~/zxvenv); without them those checks are skipped, and said so.
"""
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
O = os.path.join(HERE, "..", "..", "patches", "buildroot", "overlay-src")
LIBWB = os.path.join(HERE, "..", "..", "libwb")
if not os.path.isdir(O):        # a copy with the headers next to it (WhyOS-Apps apps/barbarious/source)
    O = HERE
    LIBWB = os.path.join(HERE, "libwb")

try:
    import zxingcpp
    from PIL import Image
except ImportError:
    zxingcpp = None

TYPES = ["qr", "datamatrix", "aztec", "pdf417", "code128", "code39", "ean13", "upca", "itf", "codabar"]


def formats():
    F = zxingcpp.BarcodeFormat
    return [F.QRCode, F.DataMatrix, F.Aztec, F.PDF417, F.Code128, F.Code39, F.EAN13, F.UPCA, F.ITF, F.Codabar]


class App:
    def __init__(self, exe, shots):
        self.exe, self.shots = exe, shots
        self.dir = tempfile.mkdtemp()

    def run(self, keys, shot=None, env=None):
        env = dict(os.environ, APP_CONFIG=self.dir, **(env or {}))
        ppm = os.path.join(self.shots or self.dir, (shot or "last") + ".ppm")
        r = subprocess.run([self.exe, keys, ppm], capture_output=True, text=True, env=env)
        if r.returncode or "runtime error" in r.stderr or "Sanitizer" in r.stderr:
            raise SystemExit("barbarious-host failed on %r:\n%s" % (keys, r.stderr))
        st = {"codes": [], "ppm": ppm, "stderr": r.stderr}
        lines = r.stdout.splitlines()
        st["screen"] = lines[0]
        for line in lines[1:]:
            k, _, v = line.partition(" ")
            if k == "code":
                st["codes"].append(v)
            else:
                st[k] = v
        return st

    def file(self, name, text=None):
        p = os.path.join(self.dir, name)
        if text is not None:
            open(p, "w", encoding="latin-1").write(text)
        return open(p, encoding="latin-1").read()

    def codes_file(self):
        return [line for line in self.file("codes.txt").splitlines() if not line.startswith("#")]


bad = 0
checks = 0
skipped = []


def check(what, got, want):
    global bad, checks
    checks += 1
    if got != want:
        bad += 1
        print("FAIL %s: want %r, got %r" % (what, want, got))


def decode(ppm, t):
    """What zxing reads in the screenshot: (bytes, symbology id), or None."""
    r = zxingcpp.read_barcodes(Image.open(ppm), formats=formats()[t])
    return (r[0].bytes, r[0].symbology_identifier) if r else None


def sym(exe, t, units, ecc=1):
    """--sym: one symbol, as a PGM; (its output, the file)."""
    out = os.path.join(tempfile.mkdtemp(), "sym.pgm")
    hx = "".join("XX" if u == 256 else "%02x" % u for u in units)
    r = subprocess.run([exe, "--sym", str(t), hx, out, str(ecc)], capture_output=True, text=True)
    if r.returncode or "runtime error" in r.stderr or "Sanitizer" in r.stderr:
        raise SystemExit("--sym failed (%d) on %s: %s" % (r.returncode, [t, hx[:40], ecc], r.stderr))
    return r.stdout.strip(), out


def b(s):
    return list(s.encode("latin-1"))


def screens(exe, shots):
    """The screens and keys, the file, and every type shown and decoded."""
    a = App(exe, shots)
    s = a.run("", shot="empty")
    check("empty list", (s["screen"], s["codes"]), ("list ", []))

    # a new code: the triangle, a name, Tab, its text, Enter
    s = a.run("\\2Guest Wi-Fi\\tWIFI:T:WPA;S:WHY2025-guest;P:example;;\\n")
    check("added", s["codes"], ["Guest Wi-Fi|qr|WIFI:T:WPA;S:WHY2025-guest;P:example;;"])
    check("saved", s["notice"], "Saved")
    check("file", a.codes_file(), ["Guest Wi-Fi\tqr\tWIFI:T:WPA;S:WHY2025-guest;P:example;;"])

    # Tab is TAB, Shift+Enter CR; the triangle picks the type (four down: Code 128)
    s = a.run("\\2Login\\tdemo\\tdemo\\N\\2\\d\\d\\d\\d\\n", shot="edit")
    check("edit: the text", (s["screen"], s["text"], s["type"]), ("edit ", "demo\\[TAB]demo\\[CR]", "Code 128"))
    s = a.run("\\2Login\\tdemo\\tdemo\\N\\2\\d\\d\\d\\d\\n\\n")
    check("TAB and CR", s["codes"][1], "Login|code128|demo\\[TAB]demo\\[CR]")
    check("file escapes", a.codes_file()[1], "Login\tcode128\tdemo\\[TAB]demo\\[CR]")
    # Ctrl with a letter, and Ctrl+] (GS)
    s = a.run("\\2\\tA\\cmB\\c]C\\n")
    check("Ctrl+M, Ctrl+]", s["codes"][2], "|qr|A\\[CR]B\\[GS]C")
    # the insert pop-up: TAB first, a filter by name, by ^ and a letter, by a word
    s = a.run("\\2\\tx\\1", shot="insert")
    check("insert: open on TAB", (s["screen"], s["text"]), ("edit insert", "x"))
    s = a.run("\\2\\tx\\1\\n")
    check("insert: TAB", s["text"], "x\\[TAB]")
    s = a.run("\\2\\t01\\1gs\\n23", shot="insert-gs")
    check("insert: GS by name", s["text"], "01\\[GS]23")
    s = a.run("\\2\\t\\1^[\\n\\1fnc\\n\\1group\\n")
    check("insert: ^[, fnc, group", s["text"], "\\[ESC]\\[FNC1]\\[GS]")
    s = a.run("\\2\\t\\1zzz", shot="insert-none")
    check("insert: nothing matches", (s["screen"], s["filter"]), ("edit insert", "zzz"))
    s = a.run("\\2\\t\\1gs\\b\\b\\e")
    check("insert: Esc", (s["screen"], s["text"]), ("edit ", ""))
    # the cursor: Left, Backspace, Fn+Backspace
    s = a.run("\\2\\tabcd\\l\\l\\bX\\f\\b")
    check("cursor", (s["text"], s["cursor"]), ("aXd", "2"))
    # an empty text cannot be saved; Esc leaves the list as it was
    s = a.run("\\2Name\\n")
    check("empty text", (s["screen"], s["error"]), ("edit ", "Type a text first"))
    s = a.run("\\2Name\\tabc\\e")
    check("Esc cancels", (s["screen"], len(s["codes"])), ("list ", 3))

    # the list, the show screen, next and previous, the log
    s = a.run("\\d", shot="list")
    check("list focus", s["focus"], "1")
    s = a.run("\\d\\n", shot="show-qr")
    check("show", (s["screen"], s["shown"], s["lit"]), ("show ", "0", "1"))
    s = a.run("\\d\\n\\r", shot="show-128")
    check("show: next", s["shown"], "1")
    s = a.run("\\d\\n\\l")
    check("show: previous wraps", s["shown"], "2")
    s = a.run("\\d\\n\\e")
    check("show: Esc to the list, on it", (s["screen"], s["focus"], s["lit"]), ("list ", "1", "0"))
    s = a.run("\\d\\n\\r\\r\\4", shot="log")
    check("log", (s["screen"], s["log"]), ("log ", "3"))
    s = a.run("\\4", shot="log-empty")
    check("log: empty", s["log"], "0")

    # options: move down, the type, delete (Keep it first)
    s = a.run("\\d\\1", shot="options")
    check("options", s["screen"], "list menu")
    s = a.run("\\d\\1\\d\\d\\d\\n")
    check("move down", [c.split("|")[0] for c in s["codes"]], ["Login", "Guest Wi-Fi", ""])
    s = a.run("\\d\\d\\1\\d\\d\\n", shot="type")
    check("type pop-up", (s["screen"], s["needs"]), ("list type", "0 0 0 0 0 1 2 2 2 2"))
    s = a.run("\\d\\1\\d\\d\\n\\d\\n")
    check("type: from Code 128 one down", s["codes"][0].split("|")[1], "code39")
    s = a.run("\\d\\1\\d\\d\\n", shot="type-login")
    check("needs of a text with TAB and CR", s["needs"], "0 0 0 0 0 1 2 2 2 2")
    s = a.run("\\d\\d\\d\\1\\d\\d\\d\\d\\n", shot="delete")
    check("delete asks", s["screen"], "list delete")
    s = a.run("\\d\\d\\d\\1\\d\\d\\d\\d\\n\\n")
    check("keep it", len(s["codes"]), 3)
    s = a.run("\\d\\d\\d\\1\\d\\d\\d\\d\\n\\d\\n")
    check("deleted", (len(s["codes"]), s["notice"]), (2, "Deleted"))

    # Quick code: typed, shown, kept until the app closes, saved with Enter
    s = a.run("\\nHELLO 123", shot="quick")
    check("quick", (s["screen"], s["text"], s["lit"]), ("quick ", "HELLO 123", "1"))
    if zxingcpp:
        check("quick: decodes", decode(s["ppm"], 0), (b"HELLO 123", "]Q1"))
    s = a.run("\\nHELLO\\2\\d\\d\\d\\d\\n", shot="quick-128")
    check("quick: Code 128", s["type"], "Code 128")
    if zxingcpp:
        check("quick 128: decodes", decode(s["ppm"], 4), (b"HELLO", "]C0"))
    s = a.run("\\nabc\\nQuick\\n")
    check("quick: saved", s["codes"][-1], "Quick|qr|abc")
    s = a.run("\\nabc\\nQuick\\e")
    check("quick: Esc from the editor goes back to it", (s["screen"], s["text"]), ("quick ", "abc"))

    # settings: each row changes and is kept
    s = a.run("\\5", shot="settings")
    check("settings", s["screen"], "settings ")
    a.run("\\5\\r\\d\\r\\r\\d\\r\\d\\r\\d\\l\\d\\l\\e")
    check("settings file", a.file("settings"), "type=1\necc=3\nquiet=5\nshift=1\nunder=0\nbright=0\n")
    s = a.run("\\d\\n")
    check("no full brightness", s["lit"], "0")
    s = a.run("\\2\\tx\\N\\n")
    check("Shift+Enter: LF", s["codes"][-1], "|datamatrix|x\\[LF]")
    a.run("\\5\\l\\d\\l\\l\\d\\l\\d\\l\\d\\r\\d\\r\\e")
    check("settings back", a.file("settings"), "type=0\necc=1\nquiet=4\nshift=0\nunder=1\nbright=1\n")

    # help on every screen
    for keys, name in (("\\6", "list"), ("\\d\\n\\6", "show"), ("\\2\\6", "edit"), ("\\n\\6", "quick"),
                       ("\\5\\6", "settings"), ("\\4\\6", "log")):
        s = a.run(keys, shot="help-" + name)
        check("help on " + name, s["screen"].split()[1:], ["help"])

    # the file: loaded with every escape, CRLF, a comment, a code without a type
    a.file("codes.txt", "# c\r\nBin\tdatamatrix\t\\xC3\\xA9\\[FNC1]01\\n\\t\\r\\\\\r\n"
           "Plain\tjust text\nPallet\tdatamatrix\t\\[FNC1]0108712345678906\\[GS]10ABC12\n")
    s = a.run("")
    check("loaded", s["codes"], ["Bin|datamatrix|\u00e9\\[FNC1]01\\[LF]\\[TAB]\\[CR]\\",
                                 "Plain|qr|just text",
                                 "Pallet|datamatrix|\\[FNC1]0108712345678906\\[GS]10ABC12"])
    a.run("\\d\\1\\d\\d\\d\\n")     # moved down: saved again
    check("saved again", a.codes_file(), ["Plain\tqr\tjust text",
                                          "Bin\tdatamatrix\t\xc3\xa9\\[FNC1]01\\[LF]\\[TAB]\\[CR]\\\\",
                                          "Pallet\tdatamatrix\t\\[FNC1]0108712345678906\\[GS]10ABC12"])
    s = a.run("\\d\\d\\d\\n", shot="show-dm-gs1")
    if zxingcpp:
        check("GS1 Data Matrix", decode(s["ppm"], 1), (b"0108712345678906\x1d10ABC12", "]d2"))

    # the text is full at 1000
    s = a.run("\\2\\t" + "x" * 1001)
    check("full", (s["error"], len(s["text"])), ("The text is full (1000 characters)", 1000))

    # what cannot be made: letters in an EAN-13
    a.file("codes.txt", "Bad\tean13\tabc\n")
    s = a.run("\\d\\n", shot="show-error")
    check("show: cannot", s["screen"], "show ")
    if zxingcpp:
        check("show: nothing to read", decode(s["ppm"], 6), None)


# every type on the show screen, as drawn, read back
SHOWN = [
    (0, "WIFI:T:WPA;S:WHY2025-guest;P:example;;", None),
    (1, "Pallet 42, row 7", None),
    (2, "Aztec: tickets and boarding passes", None),
    (3, "PDF417\tholds\rcontrol characters", None),
    (4, "demo\tdemo\r", None),
    (5, "WHY-0042", None),
    (5, "lower\tcase", None),
    (6, "871234567890", b"8712345678906"),
    (7, "03600029145", b"0036000291452"),
    (8, "12345", b"012345"),
    (9, "40156", b"A40156A"),
]


def shown(exe, shots):
    if not zxingcpp:
        skipped.append("the screens decoded (no zxing-cpp)")
        return
    a = App(exe, shots)
    esc = {9: "\\[TAB]", 13: "\\[CR]"}
    a.file("codes.txt", "".join("%d\t%s\t%s\n" % (i, TYPES[t], "".join(esc.get(ord(c), c) for c in text))
                                for i, (t, text, _) in enumerate(SHOWN)))
    for i, (t, text, want) in enumerate(SHOWN):
        for under in (1, 0):
            a.file("settings", "under=%d\n" % under)
            s = a.run("\\d" * (i + 1) + "\\n", shot="show-%d-%s%s" % (i, TYPES[t], "" if under else "-bare"))
            got = decode(s["ppm"], t)
            check("shown %s %r (text under: %d)" % (TYPES[t], text, under), got and got[0],
                  want or text.encode("latin-1"))
    # QR's four levels, and a big one at 2 px a module
    for ecc in range(4):
        a.file("settings", "ecc=%d\n" % ecc)
        s = a.run("\\d\\n")
        got = decode(s["ppm"], 0)
        check("QR level %d" % ecc, got and got[0], SHOWN[0][1].encode())
    rnd = random.Random(2)
    big = "".join(rnd.choice("abcdefghijklmnopqrstuvwxyz0123456789 ") for _ in range(1000))
    a.file("codes.txt", "Big\tqr\t%s\nBig\tdatamatrix\t%s\nBig\taztec\t%s\nBig\tpdf417\t%s\n" % (
        big, big, big, big[:600]))
    a.file("settings", "")
    for i, t in enumerate((0, 1, 2, 3)):
        s = a.run("\\d" * (i + 1) + "\\n", shot="show-big-" + TYPES[t])
        got = decode(s["ppm"], t)
        check("big %s" % TYPES[t], got and got[0], (big[:600] if t == 3 else big).encode())


# keys that change a little at a time: focus moves (the list scrolled),
# the options pop-up and its focus, codes shown one after the other (two
# QR Codes of one size), the type pop-up, Quick code typed and its insert
# pop-up filtered, the editor (its fields, the type changed: the
# preview), the settings (a choice, the slider), the help and the delete
# dialog
PARTIAL_KEYS = ("\\d\\d\\d\\d\\d\\d\\d\\d\\u\\u"
                "\\1\\d\\d\\u\\e"
                "\\u\\u\\u\\u\\u\\n\\r\\r\\r\\l\\2\\d\\d\\u\\e\\e"
                "\\u\\u\\u\\u\\u\\u\\nAB1\\b\\1c\\d\\e\\2\\d\\n\\e"
                "\\d\\d\\3xy\\b\\u\\d\\d\\r\\r\\l\\e"
                "\\5\\d\\r\\d\\r\\r\\l\\d\\d\\r\\e"
                "\\6\\e"
                "\\1\\d\\d\\d\\d\\d\\n\\d\\u\\e")
CODES = ("Ticket A\tqr\tAAAA-1111\nTicket B\tqr\tBBBB-2222\nPallet\tdatamatrix\tPallet 42\n"
         "Login\tcode128\tdemo\\[TAB]demo\\[CR]\nItem\tean13\t871234567890\nBoarding\taztec\tAZ 123\n"
         "Box\tpdf417\tbox 7\nPart\tcode39\tWHY-0042\nTag\titf\t12345\nLib\tcodabar\t40156\n")


def partial(exe):
    """Only the bands that changed drawn (libwb wb_frame, wb_bands): every
    frame of PARTIAL_KEYS the same as drawn whole (WB_FULL=1), in the whole
    picture and in bands."""
    def frames(extra):
        a = App(exe, "")
        a.file("codes.txt", CODES)
        pre = os.path.join(a.dir, "f")
        s = a.run(PARTIAL_KEYS, env=dict(extra, BB_FRAMES=pre, BARBARIOUS_UI_TIMING="1"))
        fs = sorted(f for f in os.listdir(a.dir) if f.startswith("f") and f.endswith(".ppm"))
        bands = [int(line.split(", ")[1].split()[0]) for line in s["stderr"].splitlines()
                 if line.startswith("barbarious: frame ")]
        return [open(os.path.join(a.dir, f), "rb").read() for f in fs], bands, s

    for mode, extra in (("whole", {}), ("bands", {"BB_BANDS": "1"})):
        part, bands, s = frames(extra)
        full, full_bands, _ = frames(dict(extra, WB_FULL="1"))
        check("partial %s: frames" % mode, (len(part), s["screen"]), (len(full), "list "))
        check("partial %s: the same as whole" % mode, [i for i in range(len(part)) if part[i] != full[i]], [])
        check("partial %s: drawn partly" % mode, (min(bands) <= 4, max(full_bands)), (True, 18))


def symbols(exe):
    """--sym: lengths and bytes beyond what a screen shows."""
    if not zxingcpp:
        skipped.append("the symbols decoded (no zxing-cpp)")
        return
    rnd = random.Random(1)
    texts = [list(range(256)), b("1"), b("A"), b("Hello, World 123.45 abc DEF")]
    for n in (10, 50, 120, 300, 700, 1000):
        texts.append([rnd.randrange(256) for _ in range(n)])
        texts.append(b("".join(rnd.choice("ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz0123456789.,")
                               for _ in range(n))))
    for t in range(4):
        for units in texts:
            out, pgm = sym(exe, t, units)
            if out.startswith("error"):
                check("%s %d units: %s" % (TYPES[t], len(units), out), t == 3 and len(units) >= 1000, True)
                continue
            got = decode(pgm, t)
            check("%s, %d units" % (TYPES[t], len(units)), got and got[0], bytes(units))
    for ecc in range(4):
        out, pgm = sym(exe, 0, b("level %d" % ecc), ecc)
        check("QR at level %d" % ecc, decode(pgm, 0)[0], b"level %d" % ecc)
    for units in (b("Login"), b("1234567890"), b("ab\x01\x02\x03cd"), b("abc1234567def"), list(range(128)),
                  b("12345"), b("123456"), b("x1234")):
        out, pgm = sym(exe, 4, units)
        check("Code 128 %r" % bytes(units)[:20], decode(pgm, 4)[0], bytes(units))
    out, _ = sym(exe, 4, b("demo\tdemo\r"))
    check("Code 128 with a Shift: 167 modules", out, "ok 167 1 0 2")
    out, _ = sym(exe, 4, b("ab\x01\x02\x03cd"))
    check("Code 128: CODE A for three", out.split()[-1], "1")
    for n in (1, 2, 3, 7, 30, 80):
        for units in ([rnd.randrange(128) for _ in range(n)], b("".join(rnd.choice("0123456789") for _ in range(n)))):
            out, pgm = sym(exe, 4, units)
            check("Code 128 random %d" % n, decode(pgm, 4)[0], bytes(units))
            out, pgm = sym(exe, 5, units)
            check("Code 39 random %d" % n, decode(pgm, 5)[0], bytes(units))
        digits = "".join(rnd.choice("0123456789") for _ in range(n))
        out, pgm = sym(exe, 8, b(digits))
        check("ITF %d digits" % n, decode(pgm, 8)[0], (("0" if n % 2 else "") + digits).encode())
    gs1 = [256] + b("0108712345678906") + [256] + b("10ABC12")
    for t, sid in ((1, "]d2"), (4, "]C1")):
        out, pgm = sym(exe, t, gs1)
        check("GS1 %s" % TYPES[t], decode(pgm, t), (b"0108712345678906\x1d10ABC12", sid))
    for t in (0, 2, 3, 5, 6):
        out, _ = sym(exe, t, gs1)
        check("no FNC1 in %s" % TYPES[t], out.startswith("error"), True)
    for t, units, why in ((6, b("12345678901"), "12 digits"), (6, b("8712345678900"), "wrong check digit"),
                          (7, b("1234567890"), "11 digits"), (8, b("12a4"), "digits only"),
                          (9, b("A1234"), "0\u2013" "9 - $ : / . + only"), (4, [200], "ASCII 0\u2013" "127 only")):
        out, _ = sym(exe, t, units)
        check("%s refuses %r" % (TYPES[t], bytes(units)), why in out, True)


def main():
    shots = sys.argv[1] if len(sys.argv) > 1 else ""
    if shots:
        os.makedirs(shots, exist_ok=True)
    exe = os.path.join(tempfile.mkdtemp(), "barbarious-host")
    san = ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]
    subprocess.run(["cc", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-DBB_HOST", *san,
                    "-I" + O, "-I" + LIBWB, "-I" + HERE, "-o", exe, os.path.join(HERE, "barbarious.c"),
                    os.path.join(HERE, "symbol.c"), os.path.join(O, "qrcodegen", "qrcodegen.c")], check=True)
    os.environ["ASAN_OPTIONS"] = "detect_leaks=0"
    screens(exe, shots)
    shown(exe, shots)
    symbols(exe)
    partial(exe)
    for s in skipped:
        print("skipped:", s)
    print("%d of %d failed" % (bad, checks))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
