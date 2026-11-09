"""whyos.testing: test an app built on the WhyOS SDK without test code in the app.

The app's own C source is built for this computer under sbsim (sandbox/test/sbsim.c): its system
calls meet what they meet on the badge (the sandbox's rules, the panel, the keypad, standby), on a
clock that only moves when the app waits. A test presses keys and moves the clock; what the screen
shows comes from the SDK's test channel (whyos/test.h) as data, its picture from the panel.

    from whyos_testing import App

    def test_red(tmp_path):
        app = App(["flashlight.c"], "flashlight", tmp=tmp_path)
        app.advance(2)
        app.press("✕")
        assert app.screen["colour"] == "#ff0000"
        assert app.backlight == 80

Each question about the app (screen, picture, settings, report) runs it from the start to the
moment the test is at: a run is deterministic, so this is the same as asking a running app, and
a test reads as a sequence of steps. Runs are kept, so asking twice costs one run.

sdk/README.md, "Testing", says more; `python3 sdk/tools/whyos-sdk test <app dir>` runs an app's
tests.
"""
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
WHYOS = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(WHYOS, "sandbox", "test"))
import sbsim  # noqa: E402

INCLUDE = [os.path.join(WHYOS, "sdk", "include"), os.path.join(WHYOS, "libwb")]

# key names as the badge's keys are labelled, and their Linux codes
KEYS = {
    "■": 59, "▲": 60, "✕": 61, "●": 62, "♣": 63, "◆": 64,
    "Esc": 1, "Enter": 28, "Tab": 15, "Backspace": 14, "Delete": 111, "Space": 57,
    "Up": 103, "Down": 108, "Left": 105, "Right": 106, "Home": 102, "End": 107,
    "PgUp": 104, "PgDn": 109,
    "↑": 103, "↓": 108, "←": 105, "→": 106,
}
MODS = {"Shift": 42, "Ctrl": 29, "Alt": 56, "Fn": 0x1d0}
_LO = "\0\0" "1234567890-=" "\0\0" "qwertyuiop[]" "\0\0" "asdfghjkl;'`" "\0" "\\zxcvbnm,./"
_HI = "\0\0" "!@#$%^&*()_+" "\0\0" "QWERTYUIOP{}" "\0\0" "ASDFGHJKL:\"~" "\0" "|ZXCVBNM<>?"

STEP_MS = 300       # a key press and the time until the next one
WHITE, RED, BLACK = (255, 255, 255), (255, 0, 0), (0, 0, 0)


def key_codes(name):
    """'Fn+Esc' -> ([464], 1); 'x' -> ([], 45); 'X' -> ([42], 45)."""
    *mods, key = name.split("+") if len(name) > 1 and "+" in name else [name]
    held = [MODS[m] for m in mods]
    if key in KEYS:
        return held, KEYS[key]
    if key == " ":
        return held, 57
    if len(key) == 1 and key in _LO[2:]:
        return held, _LO.index(key)
    if len(key) == 1 and key in _HI[2:]:
        return held + [MODS["Shift"]], _HI.index(key)
    raise ValueError("no such key: %r" % name)


_built = {}


def build(sources, flags=(), out_dir=None):
    """The app's badge code for this computer, under sbsim; built once per set of sources."""
    sources = [os.path.abspath(s) for s in sources]
    h = hashlib.sha1()
    for s in sources + list(flags):
        h.update(s.encode())
        if os.path.exists(s):
            with open(s, "rb") as f:
                h.update(f.read())
    for d in INCLUDE:
        for root, _, files in os.walk(d):
            for n in sorted(files):
                if n.endswith(".h"):
                    h.update(os.path.getmtime(os.path.join(root, n)).hex().encode())
    key = h.hexdigest()[:16]
    if key not in _built:
        out_dir = out_dir or os.path.join(os.environ.get("TMPDIR", "/tmp"), "whyos-testing")
        os.makedirs(out_dir, exist_ok=True)
        exe = os.path.join(out_dir, "app-" + key)
        if not os.path.exists(exe):
            sbsim.build(exe + ".tmp", sources, flags=list(flags), include=INCLUDE)
            os.replace(exe + ".tmp", exe)
        _built[key] = exe
    return _built[key]


class App:
    """An app on the computer, with its badge code under sbsim.

    sources   the app's C files (the SDK comes with whyos.h)
    app_id    its id: APP_ID, and the places APP_CONFIG and the rest
    settings  its settings before the start (key -> value), as APP_CONFIG/settings holds them
    files     {path in APP_DIR or an absolute badge path: text or bytes} before the start
    env       more environment (SBSIM_STANDBY="10000-20000", PERMISSIONS=..., ...)
    """

    def __init__(self, sources, app_id, tmp, settings=None, files=None, env=None, flags=()):
        self.exe = build(sources, flags)
        self.id = app_id
        self.tmp = str(tmp)
        self.settings_before = dict(settings or {})
        self.files = dict(files or {})
        self.env = dict(env or {})
        self.keys = []          # (ms, code): a press of 100 ms each
        self.t = 500            # the virtual ms the test is at; the app starts at 0
        self._runs = {}

    # ---- what the test does ----------------------------------------------------------------

    def press(self, *names):
        """Keys one after another, STEP_MS apart: 'Enter', '◆', 'Fn+Esc', 'x', 'Up'."""
        for name in names:
            held, code = key_codes(name)
            for i, m in enumerate(held):
                self.keys.append((self.t + i, m))
            self.keys.append((self.t + len(held) + 10, code))
            self.t += STEP_MS
        return self

    def type(self, text):
        """Text typed on the keypad, a key per character."""
        for ch in text:
            self.press(ch)
        return self

    def advance(self, seconds):
        """The clock moves on: the app waits that long."""
        self.t += int(seconds * 1000)
        return self

    # ---- what the app shows and did ------------------------------------------------------

    def run(self, at=None):
        """The app run to the virtual ms `at` (now by default): (report, out dir, root)."""
        at = self.t if at is None else at
        key = (tuple(self.keys), at)
        if key not in self._runs:
            n = len(self._runs)
            root = os.path.join(self.tmp, "%s-%d" % (self.id, n))
            out = os.path.join(root, "out")
            if os.path.exists(root):
                shutil.rmtree(root)
            sbsim.tree(root, self.id)
            os.makedirs(os.path.join(root, "tmp", "app-" + self.id, "test"))
            conf = os.path.join(root, "root", ".whyos", "config", self.id)
            if self.settings_before:
                with open(os.path.join(conf, "settings"), "w") as f:
                    f.write("".join("%s=%s\n" % kv for kv in self.settings_before.items()))
            for path, data in self.files.items():
                p = os.path.join(root, path.lstrip("/")) if path.startswith("/") else \
                    os.path.join(root, "mnt", "sd", "badge", "apps", self.id, path)
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "wb" if isinstance(data, bytes) else "w") as f:
                    f.write(data)
            r = sbsim.run(self.exe, root, out, self.id, end=at, keys=self.keys, shots=[at], **self.env)
            self._runs[key] = (r, out, root, at)
        return self._runs[key]

    @property
    def report(self):
        """sbsim's report: status, light, touches, refused, torn, heap_peak, ..."""
        return self.run()[0]

    @property
    def screen(self):
        """What the screen shows now, as the SDK describes it (whyos/test.h)."""
        r, out, root, at = self.run()
        path = os.path.join(root, "tmp", "app-" + self.id, "test", "screen")
        if not os.path.exists(path):
            return None
        with open(path) as f:
            return json.load(f)

    @property
    def closed(self):
        """Did the app end by now?"""
        r = self.run()[0]
        return r["end_ms"] < self.t

    @property
    def pixels(self):
        """The panel now: 720 x 720 RGB bytes."""
        r, out, root, at = self.run()
        return sbsim.pixels(out, at)

    def pixel(self, x, y):
        return sbsim.pixel(self.pixels, x, y)

    def shot(self, path):
        """The panel now as a PNG."""
        png(self.pixels, path)
        return path

    @property
    def settings(self):
        """APP_CONFIG/settings now (key -> value)."""
        r, out, root, at = self.run()
        path = os.path.join(root, "root", ".whyos", "config", self.id, "settings")
        if not os.path.exists(path):
            return {}
        with open(path) as f:
            return dict(l.rstrip("\n").split("=", 1) for l in f if "=" in l)

    @property
    def backlight(self):
        """The backlight's duty the app set last, or None (the system's)."""
        lights = self.report["light"]
        return lights[-1][1] if lights else None

    def path(self, badge_path):
        """A badge path in the simulated root, after the run to now."""
        r, out, root, at = self.run()
        return os.path.join(root, badge_path.lstrip("/"))

    # ---- the standard checks -------------------------------------------------------------

    def check(self):
        """What every app must keep to, after the run to now: no access outside its places, no
        torn frame (a copy still on its way when the buffer took other rows), the panel drawn
        in bands equal to it drawn whole."""
        r = self.report
        assert r["refused"] == [] and r["ioctls_refused"] == 0, "refused: %r" % (r["refused"],)
        assert r["torn"] == 0, "torn frame: %s" % r.get("first_torn")
        whole = App.__new__(App)
        whole.__dict__.update(self.__dict__)
        whole.env = dict(self.env, WB_FULL="1")
        whole._runs = {}
        assert whole.pixels == self.pixels, "drawn in bands, the screen differs from it drawn whole"


def unit(test_c, logic, tmp, flags=("-lm",)):
    """A unit test file (whyos/check.h) built with the app's logic files, not its main(), and
    run; its output, or AssertionError with it when a check failed."""
    exe = os.path.join(str(tmp), os.path.basename(test_c)[:-2])
    subprocess.run(["cc", "-O1", "-g", "-Wall", "-fsanitize=address,undefined", "-I" + INCLUDE[0], test_c] +
                   list(logic) + ["-o", exe] + list(flags), check=True)
    p = subprocess.run([exe], capture_output=True, text=True)
    assert p.returncode == 0, p.stdout + p.stderr
    return p.stdout


def png(rgb, path, w=720, h=720):
    """RGB bytes as a PNG (no Pillow needed)."""
    raw = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
