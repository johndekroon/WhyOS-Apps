# SPDX-License-Identifier: MIT
"""The badge's general settings (docs/SETTINGS.md), as the setting command
reads and writes them: the same two files and the same rules as
patches/buildroot/overlay-src/settings.h.

    import setting
    setting.get("name")                  # "John"; None when there is none
    setting.get("callsign", "")          # or the default given
    setting.set("callsign", "PD0ABC", menu="Call sign")
    setting.unset("callsign")            # back to its default, or gone
    setting.list()                       # [(key, menu, label, value), ...]

Values are strings of one line, up to 127 bytes. set() takes menu= as the
command takes -m and -h: a label shows the setting under Settings >
General, False hides it, None (the default) keeps how it is shown; a new
setting is not shown.
"""
import os

DEFAULTS = "/usr/share/badge/settings"  # in the rootfs: the defaults
FILE = "/etc/badge/settings"  # in flash: what was set, wins over a default
MAX = 32
_HEAD = (b"# Badge settings (docs/SETTINGS.md): key, menu or -, label, value;\n"
         b"# tab-separated. Written by the setting command and the menu.\n")
_KEY_CHARS = b"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"


def _key_ok(k):
    return 0 < len(k) < 32 and all(c in _KEY_CHARS for c in k)


def _text(b):
    # settings.h cuts a field at a byte count, which can split a character
    for cut in range(4):
        try:
            return b[:len(b) - cut].decode()
        except UnicodeError:
            pass
    return ""


def _read(rows, path, was_set):
    # a row: [key, menu, label, value, set], fields as bytes
    try:
        with open(path, "rb") as f:
            data = f.read(8191)
    except OSError:
        return
    for line in data.split(b"\n"):
        if not line or line[0] == 35:  # '#'
            continue
        p = line.split(b"\t", 3) + [b"", b"", b""]
        key = p[0][:31]
        if not _key_ok(key):
            continue
        row = [key, p[1][:15] == b"menu", p[2][:39], p[3][:127], was_set]
        for i, r in enumerate(rows):
            if r[0] == key:
                rows[i] = row
                break
        else:
            if len(rows) < MAX:
                rows.append(row)


def _load():
    rows = []
    _read(rows, DEFAULTS, False)
    _read(rows, FILE, True)
    return rows


def _find(rows, key):
    for r in rows:
        if r[0] == key:
            return r
    return None


def _save(rows):
    d = FILE[:FILE.rfind("/")]
    try:
        os.mkdir(d)
    except OSError:
        pass
    tmp = FILE + ".new"
    try:
        with open(tmp, "wb") as f:
            f.write(_HEAD)
            for key, menu, label, value, was_set in rows:
                if was_set:
                    f.write(b"\t".join((key, b"menu" if menu else b"-", label, value)) + b"\n")
            f.flush()  # fsync, as the command does
    except OSError:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise
    os.rename(tmp, FILE)


def get(key, default=None):
    """The value of key, or default when there is no such setting."""
    r = _find(_load(), key.encode())
    return default if r is None else _text(r[3])


def set(key, value, menu=None):
    """Set key to value (str() of it). menu: a label to show it under
    Settings > General, True to show it, False not to, None to keep."""
    k = key.encode()
    v = str(value).encode()
    if not _key_ok(k):
        raise ValueError("a key is letters, digits, '.', '_' and '-' (up to 31)")
    if b"\n" in v or len(v) > 127:
        raise ValueError("a value is one line of up to 127 bytes")
    label = None
    if isinstance(menu, str):
        label = menu.replace("\t", "").replace("\n", "").encode()
        if len(label) > 39:
            raise ValueError("a label is up to 39 bytes")
    rows = _load()
    r = _find(rows, k)
    if r is None:
        if len(rows) >= MAX:
            raise ValueError("too many settings")
        r = [k, False, k, b"", True]
        rows.append(r)
    r[3] = v
    r[4] = True
    if label is not None:
        r[1], r[2] = True, label
    elif menu is not None:
        r[1] = bool(menu)
    _save(rows)


def unset(key):
    """Back to its default, or gone when it has none."""
    rows = _load()
    r = _find(rows, key.encode())
    if r is not None and r[4]:
        r[4] = False
        _save(rows)


def list():
    """Every setting: (key, menu, label, value), menu True when it is
    shown under Settings > General."""
    return [(_text(r[0]), r[1], _text(r[2]), _text(r[3])) for r in _load()]
