"""The store's screens (src/ui.c) on the host: store-ui-host takes its keys
from a script and writes the screens it is asked for as PPM.

    make -C client test        (Linux: builds store-ui-host too)

The same local HTTPS source as test_client.py, with an icon and two
screenshots for the "hello" app. SHOTS=dir keeps the screens as PNG.
"""
import os
import shutil
import subprocess

import pytest

import test_client as tc

UI = os.path.join(tc.CLIENT, "store-ui-host")


@pytest.fixture
def env(tmp_path):
    if not os.path.exists(UI):
        pytest.skip("no store-ui-host: make -C client ui-host")
    Image = pytest.importorskip("PIL.Image")
    e = tc.Env(str(tmp_path))
    tc.write_app(e.repo, "hello", tc.HELLO.format(version="1.0.0", build=1), tc.hello_files("1.0.0"))
    tc.write_app(e.repo, "tiny", tc.TINY, {"tiny": "#!/bin/sh\necho tiny\n", "LICENSES/tiny.txt": "MIT\n"})
    d = os.path.join(e.repo, "apps", "hello")
    Image.new("RGBA", (256, 256), (97, 242, 255, 255)).save(os.path.join(d, "icon.png"))
    os.makedirs(os.path.join(d, "screenshots"))
    for n, c in ((1, (242, 94, 149)), (2, (255, 251, 150))):
        Image.new("RGB", (720, 720), c).save(os.path.join(d, "screenshots", "%d.png" % n))
    e.publish()
    e.serve()
    yield e
    e.stop()


def ui(e, keys, *args):
    """Run the screens with keys (one action per line); the PPMs it wrote."""
    out = os.path.join(e.tmp, "shots")
    os.makedirs(out, exist_ok=True)
    lines = [("shot %s/%s.ppm" % (out, k[5:])) if k.startswith("shot ") else k for k in keys]
    script = os.path.join(e.tmp, "keys.txt")
    with open(script, "w") as f:
        f.write("\n".join(lines) + "\n")
    env = dict(os.environ, HOME=e.home, STORE_SD=e.sd, STORE_FLASH_APPS=e.flash, STORE_TMP=e.tmpdir,
               STORE_SYS_SOURCES=os.path.join(e.etc, "sources.d"),
               STORE_RELEASE=os.path.join(e.etc, "whyos-release"), STORE_UI_SCRIPT=script,
               ASAN_OPTIONS="detect_leaks=0")
    r = subprocess.run([UI, *args], env=env, capture_output=True, text=True, timeout=300)
    print(r.stderr)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "AddressSanitizer" not in r.stderr and "runtime error" not in r.stderr, r.stderr
    keep = os.environ.get("SHOTS")
    shots = {}
    for k in keys:
        if k.startswith("shot "):
            p = os.path.join(out, k[5:] + ".ppm")
            assert os.path.exists(p), k
            shots[k[5:]] = p
            if keep:
                from PIL import Image
                os.makedirs(keep, exist_ok=True)
                Image.open(p).save(os.path.join(keep, k[5:] + ".png"))
    return shots


def pixel(path, x, y):
    from PIL import Image
    return Image.open(path).convert("RGB").getpixel((x, y))


def test_browse_search_install_open(env):
    ui(env, ["shot discover",
             "down", "enter", "shot type",            # Browse > Apps
             "enter", "shot category",                # Utilities
             "f1", "shot category-sorted",
             "esc", "esc",
             "up", "enter", "type hel", "shot search",
             "down", "enter", "shot app",             # Hello's page
             "enter", "shot installed",               # Install
             "down", "down", "down", "enter", "shot description", "esc",
             "down", "enter", "shot screenshot", "right", "shot screenshot2", "esc",
             "esc", "esc", "esc"])
    assert os.path.isfile(env.reg("hello"))
    assert os.path.isdir(env.app("hello"))


def test_open_asks_the_menu(env):
    env.store("install", "tiny")
    ui(env, ["enter", "type tiny", "down", "enter", "shot tiny", "enter"])
    with open(os.path.join(env.tmpdir, "store-open")) as f:
        assert f.read() == "tiny"


def test_my_apps_update_and_remove(env):
    env.store("install", "hello")
    tc.write_app(env.repo, "hello", tc.HELLO.format(version="1.1.0", build=2), tc.hello_files("1.1.0"))
    env.publish()
    env.store("update")
    shots = ui(env, ["shot myapps", "enter", "shot updated",
                     "down", "down", "enter", "shot app", "down", "enter", "shot ask", "down", "enter",
                     "shot removed"],
               "--my-apps")
    assert shots
    tc.nothing_left(env, "hello")


def test_settings_and_licences(env):
    ui(env, ["down"] * 6 + ["enter", "shot settings", "enter", "shot source", "esc",
                            "down", "down", "enter", "shot keep", "esc",
                            "up", "up", "up", "enter", "shot app"] + ["down"] * 6 +
       ["shot app-low", "up", "up", "enter", "shot licences", "enter", "shot licence", "esc", "esc",
        "down", "enter", "shot versions"])
    assert os.path.exists(os.path.join(env.home, ".whyos", "store", "keep-packages"))


def test_no_network(env):
    env.stop()
    shots = ui(env, ["shot offline"])
    assert shots
