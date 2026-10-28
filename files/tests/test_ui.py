"""Files' screens (src/ui.c, src/main.c) on the host: files-ui-host takes
its keys from a script and writes the screens it is asked for as PPM.

    make -C files test        (builds files-ui-host where why2025-linux is)

The drives are disks.c's fixed set for the host (an SD card in the badge
layout with its FAT at /media/sd, a USB stick, the badge's own); the
folders are made here. SHOTS=dir keeps the screens as PNG (needs PIL).

Colours are the design's (Neon Orbit tokens), read back from RGB565.
"""
import os
import platform
import subprocess

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
UI = os.path.join(HERE, "..", "files-ui-host-" + platform.system())

CYAN = (0x61, 0xF2, 0xFF)
PINK = (0xF2, 0x5E, 0x95)
TEXT_HI = (0xF2, 0xF2, 0xFA)
TEXT_MID = (0xC8, 0xC8, 0xDC)


class Shot:
    def __init__(self, path):
        with open(path, "rb") as f:
            data = f.read()
        head = data.split(b"\n", 3)
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


def reddish(p):
    return p[0] > 180 and p[1] < 120 and p[2] < 140


def scene_pink(p):
    """the scene behind the screen, tinted pink top left"""
    return p[0] > p[1] + 40 and p[0] > 60


@pytest.fixture
def env(tmp_path):
    if not os.path.exists(UI):
        pytest.skip("no %s: make -C files ui-host" % os.path.basename(UI))
    home = tmp_path / "home"
    (home / "photos").mkdir(parents=True)
    (home / "notes.md").write_text("# Notes\n\nBring the badge.\n")
    (home / "camp-map.png").write_bytes(b"\x89PNG\r\n\x1a\n" + bytes(3000))
    (home / "firmware.bin").write_bytes(bytes(range(256)) * 4096)
    apps = tmp_path / "apps"
    for name, conf in (("write", 'NAME="Write"\nEXEC="run"\nOPENS="md txt"\n'),
                       ("imgview", 'NAME="Picture viewer"\nEXEC="run"\nOPENS="png jpg"\n'),
                       ("hexview", 'NAME="Hex viewer"\nEXEC="run"\nOPENS_ALSO="*"\n')):
        (apps / name).mkdir(parents=True)
        (apps / name / "app.conf").write_text(conf)
        (apps / name / "run").write_text("#!/bin/sh\n")
        (apps / name / "run").chmod(0o755)
    (tmp_path / "config").mkdir()
    e = dict(os.environ, FILES_APPS=str(apps), FILES_LOG=str(tmp_path / "files.log"),
             FILES_OPEN=str(tmp_path / "open"), FILES_WHERE=str(tmp_path / "where"),
             APP_CONFIG=str(tmp_path / "config"), ASAN_OPTIONS="detect_leaks=1")
    return tmp_path, home, e


def ui(env, keys, *args):
    """Run Files with keys (one action per line); the shots it took, by name."""
    tmp, home, e = env
    out = tmp / "shots"
    out.mkdir(exist_ok=True)
    lines = [("shot %s/%s.ppm" % (out, k[5:])) if k.startswith("shot ") else k for k in keys]
    script = tmp / "keys.txt"
    script.write_text("\n".join(lines) + "\n")
    r = subprocess.run([UI, *[str(a) for a in args]], env=dict(e, FILES_UI_SCRIPT=str(script)),
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


def where(env):
    tmp = env[0]
    return tuple((tmp / "where").read_text().split("\n")[:5])


def test_folder(env):
    _, home, _ = env
    s = ui(env, ["shot folder"], home / "notes.md")["folder"]
    assert s.count(0, 0, 200, 200, scene_pink) > 2000               # the scene behind it
    assert s.count(54, 96, 200, 140, is_c(TEXT_HI)) > 200          # the title: "Files"
    assert s.count(54, 72, 400, 90, is_c(CYAN, 40)) > 100          # the eyebrow: where it is
    # the columns' names, then notes.md with the focus: its row cyan, in the panel
    assert s.count(100, 150, 680, 182, is_c((0x9A, 0x9A, 0xB6), 30)) > 50
    assert s.count(48, 184, 672, 646, is_c(CYAN, 40)) > 300
    assert s.count(540, 670, 620, 710, is_c(TEXT_MID, 40)) > 20     # "3 / 4" in the hint bar
    assert where(env)[:3] == ("home", str(home), "notes.md")


def test_drives(env):
    _, home, _ = env
    s = ui(env, ["f3", "shot drives"], home)["drives"]
    assert where(env)[0] == "Drives"
    # the drive the folder is on: Memory (/tmp) or System (/)
    assert where(env)[2] == ("Memory" if str(home).startswith("/tmp/") else "System")
    assert s.count(54, 96, 300, 140, is_c(TEXT_HI)) > 200
    s = ui(env, ["f3", "up", "up", "up", "up", "up", "enter"], home)
    assert where(env)[:2] == ("SD card, Partition 1", "/media/sd")


def test_enter_opens_with_the_app(env):
    tmp, home, _ = env
    ui(env, ["enter"], home / "notes.md")
    assert (tmp / "open").read_text() == "%s\n%s\n" % (tmp / "apps" / "write", home / "notes.md")


def test_open_with_offers_the_others(env):
    tmp, home, _ = env
    # notes.md: Open with... lists Write and the Hex viewer (OPENS_ALSO "*"), by name
    s = ui(env, ["f1", "enter", "shot openwith", "enter"], home / "notes.md")
    assert s["openwith"].count(72, 100, 648, 680, is_c(CYAN, 40)) > 50     # the focused row
    assert (tmp / "open").read_text().startswith(str(tmp / "apps" / "hexview") + "\n")


def test_one_app_for_it_opens_it(env):
    tmp, home, _ = env
    ui(env, ["enter"], home / "firmware.bin")                # only the Hex viewer takes .bin
    assert (tmp / "open").read_text().startswith(str(tmp / "apps" / "hexview") + "\n")


def test_no_app_for_it(env):
    tmp, home, _ = env
    import shutil
    shutil.rmtree(tmp / "apps" / "hexview")
    s = ui(env, ["enter", "shot noapp"], home / "firmware.bin")["noapp"]
    assert s.count(48, 600, 680, 646, reddish) > 20          # "No app opens this kind of file: ..."
    assert not (tmp / "open").exists()


def test_options_rename_delete(env):
    tmp, home, _ = env
    s = ui(env, ["f1", "shot options", "down", "enter", "shot rename", "bs", "bs", "bs", "bs", "bs",
                 "type todo", "enter", "f1", "up", "enter", "shot delete", "esc"], home / "notes.md")
    assert (home / "todo.md").exists() and not (home / "notes.md").exists()
    assert s["options"].count(72, 100, 648, 680, reddish) > 20        # Delete, last, in red
    assert s["rename"].count(104, 300, 616, 420, is_c(CYAN, 40)) > 20  # the field
    assert s["delete"].count(104, 300, 616, 600, reddish) > 20        # Delete in red
    assert where(env)[2] == "todo.md"


def test_copy_paste_and_the_tag(env):
    tmp, home, _ = env
    s = ui(env, ["f1", "down", "down", "enter", "shot copied", "up", "up", "up", "enter", "f1", "enter",
                 "shot pasted"], home / "notes.md")
    assert (home / "photos" / "notes.md").read_text() == (home / "notes.md").read_text()
    assert s["copied"].count(48, 600, 680, 646, is_c(PINK, 40)) > 30   # "Copied: notes.md"
    assert "cp -a" in (tmp / "files.log").read_text()
    assert where(env)[:3] == ("photos", str(home / "photos"), "notes.md")


def test_settings_help_log(env):
    tmp, home, _ = env
    (home / ".profile").write_text("x")
    s = ui(env, ["f5", "shot settings", "enter", "shot on", "esc", "f6", "shot help", "esc", "f4", "shot log",
                 "esc"], home)
    assert s["log"].count(54, 150, 400, 200, is_c(TEXT_MID, 30)) > 20      # "No log yet."
    assert (tmp / "config" / "settings").read_text() == "hidden=1\n"
    # the switch: off, then on (cyan)
    assert s["settings"].count(600, 180, 656, 210, is_c(CYAN, 40)) < 50
    assert s["on"].count(600, 180, 656, 210, is_c(CYAN, 40)) > 400
    assert s["help"].count(110, 40, 160, 200, is_c((0xC4, 0x5C, 0xFF), 60)) > 5    # the diamond
    assert where(env)[4] == "list"
    ui(env, [], home / ".profile")              # the next start: hidden files show
    assert where(env)[2] == ".profile"


def test_del_and_pages(env):
    tmp, home, _ = env
    for i in range(30):
        (home / ("f%02d.txt" % i)).write_text("x")
    s = ui(env, ["code 109", "shot paged", "code 111", "shot del", "enter"], home / "camp-map.png")
    # photos/, camp-map.png, f00..f29, firmware.bin, notes.md: a page is 8 down
    assert where(env)[2] == "f07.txt"
    assert s["del"].count(104, 300, 616, 600, reddish) > 20          # Delete? with Delete in red
    assert (home / "f07.txt").exists()                               # Cancel is first: nothing gone
    # the scroll bar's thumb in the panel's right edge
    assert s["paged"].count(660, 184, 668, 646, is_c(CYAN, 40)) > 40
    ui(env, ["code 111", "down", "enter"], home / "f07.txt")
    assert not (home / "f07.txt").exists()


def test_popups_in_bands(env):
    """no room for a whole frame: the pop-ups in bands, unblurred, read nothing outside the band"""
    tmp, home, e = env
    e["FILES_BANDS_ONLY"] = "1"
    s = ui(env, ["f1", "shot options", "esc", "f6", "shot help", "esc", "code 111", "shot del"], home / "notes.md")
    assert s["options"].count(72, 100, 648, 680, reddish) > 20        # Delete, last, in red
    assert s["del"].count(104, 300, 616, 600, reddish) > 20


def test_focus_moves_drawn_partly(env):
    """a focus that moves draws its two rows and the hint bar again: the
    screen is the same as drawn whole every time"""
    tmp, home, e = env
    for i in range(30):
        (home / ("f%02d.txt" % i)).write_text("x")
    keys = ["down", "shot a", "down", "shot b", "up", "up", "up", "shot c", "f1", "shot d", "down", "shot d2",
            "down", "shot d3", "esc",
            "shot e", "down", "down", "down", "down", "down", "down", "shot f", "code 109", "shot g",
            "up", "shot h"]
    part = {k: v.px for k, v in ui(env, keys, home / "f03.txt").items()}
    whole = {k: v.px for k, v in ui((tmp, home, dict(e, WB_FULL="1")), keys, home / "f03.txt").items()}
    assert [k for k in part if part[k] != whole[k]] == []


def test_esc_closes_at_the_top(env):
    tmp, home, _ = env
    ui(env, ["esc", "esc", "esc", "esc", "esc", "esc", "esc", "esc", "esc", "esc", "esc", "esc"], home)
    assert not (tmp / "open").exists()
