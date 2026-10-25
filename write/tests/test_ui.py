"""Write's screens (src/ui.c, src/main.c) on the host: write-ui-host takes
its keys from a script and writes the screens it is asked for as PPM.

    make -C write test        (builds write-ui-host where why2025-linux is)

Every run sets WRITE_UI_CHECK: each frame drawn band by band (only the bands
whose contents changed) must equal the same frame drawn whole, so a band
that should have been redrawn and was not fails the test. SHOTS=dir keeps
the screens as PNG (needs PIL).

Colours are the design's (design/write/canvas, Neon Orbit tokens), read
back from RGB565: a channel is within 8 of the token.
"""
import os
import subprocess

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
UI = os.path.join(HERE, "..", "write-ui-host")

VOID = (0x0A, 0x0A, 0x14)
SURFACE1 = (0x13, 0x13, 0x1F)
CYAN = (0x61, 0xF2, 0xFF)
PINK = (0xF2, 0x5E, 0x95)
YELLOW = (0xFF, 0xFB, 0x96)
TEXT_HI = (0xF2, 0xF2, 0xFA)

NOTES = """## Hacker camp notes

Bring the badge **fully charged** and pack a *spare* USB-C cable.

### Packing list
- Badge and lanyard
- Headlamp
"""


class Shot:
    def __init__(self, path):
        with open(path, "rb") as f:
            data = f.read()
        head, self.w, self.h = data.split(b"\n", 3), 0, 0
        self.w, self.h = map(int, head[1].split())
        self.px = head[3]

    def at(self, x, y):
        i = (y * self.w + x) * 3
        return tuple(self.px[i:i + 3])

    def near(self, x, y, c, tol=8):
        return all(abs(a - b) <= tol for a, b in zip(self.at(x, y), c))

    def count(self, x0, y0, x1, y1, pred):
        return sum(1 for y in range(y0, y1) for x in range(x0, x1) if pred(self.at(x, y)))


def is_c(c, tol=24):
    return lambda p: all(abs(a - b) <= tol for a, b in zip(p, c))


@pytest.fixture
def env(tmp_path):
    if not os.path.exists(UI):
        pytest.skip("no write-ui-host: make -C write ui-host")
    home = tmp_path / "home"
    home.mkdir()
    (home / "camp-notes.md").write_text(NOTES)
    (home / "blink.py").write_text('import time\n\ndef blink(times=3):\n    print("blink")\n')
    e = dict(os.environ, WRITE_HOME=str(home), WRITE_CLIP=str(tmp_path / "clip"),
             WRITE_LOG=str(tmp_path / "log"), APP_CONFIG=str(tmp_path / "config"),
             WRITE_RUN_OUT=str(tmp_path / "run"), WRITE_UI_CHECK="1",
             ASAN_OPTIONS="detect_leaks=1")
    return tmp_path, home, e


def ui(env, keys, *args):
    """Run Write with keys (one action per line); the shots it took, by name."""
    tmp, home, e = env
    out = tmp / "shots"
    out.mkdir(exist_ok=True)
    lines = [("shot %s/%s.ppm" % (out, k[5:])) if k.startswith("shot ") else k for k in keys]
    script = tmp / "keys.txt"
    script.write_text("\n".join(lines) + "\n")
    r = subprocess.run([UI, *[str(a) for a in args]], env=dict(e, WRITE_UI_SCRIPT=str(script)),
                       capture_output=True, text=True, timeout=300)
    print(r.stderr)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "AddressSanitizer" not in r.stderr and "runtime error" not in r.stderr, r.stderr
    shots = {}
    for k in keys:
        if k.startswith("shot "):
            p = out / (k[5:] + ".ppm")
            assert p.exists(), k
            shots[k[5:]] = Shot(p)
            keep = os.environ.get("SHOTS")
            if keep:
                from PIL import Image
                os.makedirs(keep, exist_ok=True)
                Image.open(p).save(os.path.join(keep, k[5:] + ".png"))
    return shots


def test_editor_rich(env):
    _, home, _ = env
    s = ui(env, ["shot main"], home / "camp-notes.md")["main"]
    assert s.near(5, 5, VOID)
    assert s.near(5, 700, SURFACE1)                    # the hint bar
    assert s.count(24, 60, 120, 82, is_c(TEXT_HI)) > 50   # the name in the document bar
    # the caret: cyan, at the text's left edge on the first line
    assert s.count(44, 106, 52, 150, is_c(CYAN, 40)) > 10
    # the heading's text is in the text area, white
    assert s.count(48, 106, 400, 150, is_c(TEXT_HI)) > 200


def test_new_document_and_save(env):
    tmp, home, _ = env
    s = ui(env, ["shot new", "type # My list", "enter", "type - milk", "enter", "type eggs",
                 "shot typed", "ctrl s", "shot saveas", "enter", "shot saved"])
    assert (home / "my-list.md").read_text() == "# My list\n- milk\n- eggs"
    assert "my-list.md" in (tmp / "log").read_text()
    # Start typing... and the markdown tips on the empty document
    assert s["new"].count(48, 170, 400, 400, is_c(CYAN, 30)) > 50
    assert s["typed"].count(48, 240, 400, 400, is_c(CYAN, 30)) == 0     # (the caret is above)
    # the edited dot before saving, none after
    assert s["typed"].count(100, 62, 200, 78, is_c(YELLOW, 30)) > 10
    assert s["saved"].count(100, 62, 260, 78, is_c(YELLOW, 30)) == 0


def test_selection_copy_paste(env):
    tmp, home, _ = env
    s = ui(env, ["down", "down", "fn ctrl right", "fn ctrl right", "fn down", "shot selection",
                 "fn up", "ctrl c", "shot copied", "esc", "alt down", "ctrl v", "ctrl s"],
           home / "camp-notes.md")
    assert (tmp / "clip").read_text() == "Bring the"
    assert (home / "camp-notes.md").read_text().endswith("- HeadlampBring the\n")
    # the pink Fn while held, and the copied label
    assert s["selection"].count(380, 680, 600, 705, is_c(PINK, 30)) > 20
    assert s["copied"].count(150, 60, 400, 82, is_c(PINK, 40)) > 20


def test_styles(env):
    _, home, _ = env
    ui(env, ["down", "down", "ctrl right", "ctrl right", "fn ctrl right", "ctrl i", "f2", "shot style",
             "esc", "ctrl 1", "ctrl s"], home / "camp-notes.md")
    assert "# Bring the *badge* **fully charged**" in (home / "camp-notes.md").read_text()


def test_markdown_view(env):
    _, home, _ = env
    s = ui(env, ["f3", "shot source", "f3"], home / "camp-notes.md")["source"]
    # line numbers in the left column and the cyan block markers
    assert s.count(24, 106, 52, 140, is_c((0x7C, 0x7C, 0x99), 30)) + s.count(24, 106, 52, 140, is_c(TEXT_HI, 30)) > 5
    assert s.count(68, 106, 110, 140, is_c(CYAN, 30)) > 20


def test_find_and_replace(env):
    _, home, _ = env
    s = ui(env, ["ctrl f", "type badge", "shot find", "down", "type device", "f2", "shot replaced",
                 "esc", "ctrl s"], home / "camp-notes.md")
    text = (home / "camp-notes.md").read_text()
    assert "Bring the device" in text and "- Device and lanyard" in text
    assert s["find"].count(48, 200, 660, 664, is_c(YELLOW, 40)) > 30      # the matches
    assert s["replaced"].count(100, 225, 400, 250, is_c(PINK, 40)) > 30    # "Replaced 2"


def test_full_screen(env):
    _, home, _ = env
    s = ui(env, ["ctrl shift f", "shot hint", "tick 4000", "shot full", "esc", "shot back"],
           home / "camp-notes.md")
    # no status bar: the text starts at the top
    assert s["full"].count(56, 36, 400, 80, is_c(TEXT_HI)) > 100
    assert s["full"].count(600, 10, 710, 40, is_c(TEXT_HI)) == 0          # no clock
    # the hint for three seconds, then gone
    assert s["hint"].count(200, 620, 520, 664, is_c((0xC4, 0x5C, 0xFF), 40)) > 5
    assert s["full"].count(200, 620, 520, 664, is_c((0xC4, 0x5C, 0xFF), 40)) == 0
    assert s["back"].near(5, 700, SURFACE1)


def test_code_view_and_run(env):
    tmp, home, _ = env
    s = ui(env, ["shot code", "f2"], home / "blink.py")["code"]
    assert (tmp / "run").read_text().startswith("1 ") and "blink.py" in (tmp / "run").read_text()
    # "import" in cyan, the line numbers on the left
    assert s.count(62, 106, 140, 133, is_c(CYAN, 30)) > 20
    assert s.count(16, 106, 48, 133, is_c(TEXT_HI, 30)) > 5


def test_dialogs(env):
    tmp, home, _ = env
    s = ui(env, ["type x", "f1", "shot options", "esc", "esc", "shot discard", "down", "down",
                 "f6", "esc", "f5", "shot settings", "enter", "esc", "f4", "shot log", "esc",
                 "ctrl o", "shot open", "esc", "esc", "shot discard2", "down", "down", "enter"],
           home / "camp-notes.md")
    assert s["options"].count(96, 100, 624, 600, is_c(CYAN, 30)) > 100      # the focused Save
    assert s["discard"].count(96, 150, 624, 300, is_c(YELLOW, 30)) > 20      # UNSAVED CHANGES
    assert s["settings"].count(500, 300, 624, 450, is_c(PINK, 30)) > 20      # the switch
    assert s["open"].count(48, 160, 672, 640, is_c(CYAN, 30)) > 50           # the focused file
    assert (tmp / "config" / "settings").read_text() == "small=1\nlineno=1\n"
    assert (home / "camp-notes.md").read_text() == NOTES                    # discarded


def test_logo_key_saves(env):
    _, home, _ = env
    ui(env, ["type note to self", "term"])
    assert (home / "untitled.md").read_text() == "note to self"


def test_small_text(env):
    _, home, _ = env
    big = ui(env, ["shot big"], home / "camp-notes.md")["big"]
    s = ui(env, ["f5", "enter", "esc", "shot small"], home / "camp-notes.md")["small"]
    # smaller text: the paragraph line sits higher with the small size
    row = lambda sh: next(y for y in range(150, 400) if sh.count(48, y, 600, y + 1, is_c(TEXT_HI)) > 3)
    assert row(s) < row(big)
