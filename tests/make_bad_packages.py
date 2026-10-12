#!/usr/bin/env python3
"""Write good and malicious .wpk files for the package checks (docs/package.md §1.2).

    tests/make_bad_packages.py <out dir>

Writes <out>/<case>.wpk and <out>/cases.json ({case: "accept" | "reject"}).
Used by tests/test_tools.py (tools/pkglib.check_package) and by the store
client's own tests (client/), so both check the same packages.
"""
import gzip
import hashlib
import io
import json
import os
import sys
import tarfile


def manifest(files):
    return {"format": 1, "id": "evil", "name": "Evil", "version": "1.0.0", "build": 1,
            "summary": "test", "description": "test", "author": "test", "license": "MIT",
            "type": "app", "category": "other", "launch": {"exec": "run"},
            "requires": {"os_api": 1, "memory_kb": 1},
            "files": {"sha256": {k: hashlib.sha256(v).hexdigest() for k, v in files.items()}}}


def entry(name, data=None, kind=None, link=""):
    info = tarfile.TarInfo(name)
    info.mtime, info.mode = 0, 0o644
    if kind is not None:
        info.type, info.linkname = kind, link
        return info, None
    if data is None:
        info.type, info.mode = tarfile.DIRTYPE, 0o755
        return info, None
    info.size = len(data)
    return info, io.BytesIO(data)


def write(path, entries, raw=False, fmt=tarfile.USTAR_FORMAT):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=fmt) as tar:
        for info, f in entries:
            tar.addfile(info, f)
    data = buf.getvalue()
    with open(path, "wb") as out:
        out.write(data if raw else gzip.compress(data, mtime=0))


def package(files, extra=(), manifest_files=None, first=True):
    m = json.dumps(manifest(manifest_files if manifest_files is not None else files)).encode()
    body = [entry("files")] + [entry("files/" + k, v) for k, v in files.items()] + list(extra)
    return ([entry("manifest.json", m)] + body) if first else (body + [entry("manifest.json", m)])


GOOD = {"run": b"#!/bin/sh\necho hi\n", "data/a.txt": b"a\n"}

CASES = {
    "good": ("accept", lambda: package(GOOD, extra=[entry("files/data")])),
    "good-reserved-dir": ("accept", lambda: package(GOOD, extra=[entry("migrate/x", b"later formats")])),
    "manifest-not-first": ("reject", lambda: package(GOOD, first=False)),
    "traversal": ("reject", lambda: package(GOOD, extra=[entry("files/../evil", b"x")])),
    "traversal-deep": ("reject", lambda: package(GOOD, extra=[entry("files/data/../../evil", b"x")])),
    "absolute": ("reject", lambda: package(GOOD, extra=[entry("/etc/evil", b"x")])),
    "symlink": ("reject", lambda: package(GOOD, extra=[entry("files/link", kind=tarfile.SYMTYPE, link="/etc")])),
    "hardlink": ("reject", lambda: package(GOOD, extra=[entry("files/hl", kind=tarfile.LNKTYPE, link="files/run")])),
    "device": ("reject", lambda: package(GOOD, extra=[entry("files/dev", kind=tarfile.CHRTYPE)])),
    "fifo": ("reject", lambda: package(GOOD, extra=[entry("files/fifo", kind=tarfile.FIFOTYPE)])),
    "case-collision": ("reject", lambda: package(dict(GOOD, **{"Run": b"x"}))),
    "fat-character": ("reject", lambda: package(dict(GOOD, **{"a:b": b"x"}))),
    "trailing-dot": ("reject", lambda: package(dict(GOOD, **{"name.": b"x"}))),
    "app-conf": ("reject", lambda: package(dict(GOOD, **{"app.conf": b"EXEC=x\n"}))),
    "wrong-hash": ("reject", lambda: package(GOOD, manifest_files=dict(GOOD, run=b"other"))),
    "not-in-manifest": ("reject", lambda: package(GOOD, manifest_files={"run": GOOD["run"]})),
    "missing-file": ("reject", lambda: package({"run": GOOD["run"]}, manifest_files=GOOD)),
}


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    cases = {}
    for name, (verdict, make) in CASES.items():
        write(os.path.join(out, name + ".wpk"), make())
        cases[name] = verdict
    write(os.path.join(out, "not-gzip.wpk"), package(GOOD), raw=True)
    cases["not-gzip"] = "reject"
    # a name ustar cannot hold: GNU tar writes a ././@LongLink entry first
    write(os.path.join(out, "long-component.wpk"), package(dict(GOOD, **{"x" * 101: b"x"})),
          fmt=tarfile.GNU_FORMAT)
    cases["long-component"] = "reject"
    with open(os.path.join(out, "cases.json"), "w") as f:
        json.dump(cases, f, indent=1, sort_keys=True)


if __name__ == "__main__":
    main()
