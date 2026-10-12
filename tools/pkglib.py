"""Shared code for the WhyOS-Apps host tools (docs/package.md, docs/repository.md).

Python 3.11+ (tomllib). jsonschema is optional: without it, schema checks are skipped.
"""
import gzip
import hashlib
import io
import json
import os
import stat
import tarfile
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MAX_ENTRIES = 4096
MAX_UNPACKED = 256 * 1024 * 1024
MAX_COMPONENT = 100
MAX_PATH = 240                      # including the "files/" prefix
BAD_CHARS = set('"*:<>?\\|')
RESERVED_TOP = {"app.conf", ".package"}


def load_toml(path):
    with open(path, "rb") as f:
        return tomllib.load(f)


def load_json(path):
    with open(path) as f:
        return json.load(f)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


def check_name(rel):
    """Entry rules of package.md §1.2 for one path below files/. Returns a list of problems."""
    problems = []
    if rel.startswith("/") or not rel:
        return ["%r: absolute or empty path" % rel]
    if len(("files/" + rel).encode()) > MAX_PATH:
        problems.append("%s: path longer than %d bytes" % (rel, MAX_PATH))
    for part in rel.split("/"):
        if part in ("", ".", ".."):
            problems.append("%s: empty, '.' or '..' component" % rel)
        if len(part.encode()) > MAX_COMPONENT:
            problems.append("%s: component longer than %d bytes" % (rel, MAX_COMPONENT))
        if any(c in BAD_CHARS or ord(c) < 32 for c in part):
            problems.append("%s: character FAT cannot store" % rel)
        if part.endswith((".", " ")):
            problems.append("%s: trailing dot or space" % rel)
    if rel.split("/")[0] in RESERVED_TOP:
        problems.append("%s: written by the store, not allowed in a package" % rel)
    return problems


def check_tree(entries):
    """entries: {rel path: size or None for directories}. Returns a list of problems."""
    problems = []
    seen = {}
    for rel in entries:
        problems += check_name(rel)
        low = rel.lower()
        if low in seen:
            problems.append("%s and %s differ only in case" % (seen[low], rel))
        seen[low] = rel
    if len(entries) > MAX_ENTRIES:
        problems.append("%d entries, at most %d" % (len(entries), MAX_ENTRIES))
    total = sum(s for s in entries.values() if s)
    if total > MAX_UNPACKED:
        problems.append("%d bytes unpacked, at most %d" % (total, MAX_UNPACKED))
    return problems


def collect(src_dirs, skip):
    """Merge directory trees (later ones win) into {rel: (abs path or None, is_exec)}.

    Rejects anything that is not a regular file or a directory."""
    out, problems = {}, []
    for base in src_dirs:
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames.sort()
            for name in sorted(dirnames + filenames):
                full = os.path.join(dirpath, name)
                rel = os.path.relpath(full, base).replace(os.sep, "/")
                if skip(rel):
                    continue
                st = os.lstat(full)
                if stat.S_ISDIR(st.st_mode):
                    out.setdefault(rel, (None, False))
                elif stat.S_ISREG(st.st_mode):
                    out[rel] = (full, bool(st.st_mode & 0o111))
                else:
                    problems.append("%s: not a regular file or directory" % rel)
    return out, problems


def write_archive(tree, out_path):
    """A reproducible files/ archive: gzip'd ustar, sorted, mtime 0, owner root, 0755/0644."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        for rel in sorted(tree):
            path, is_exec = tree[rel]
            info = tarfile.TarInfo("files/" + rel)
            info.mtime = 0
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            if path is None:
                info.type = tarfile.DIRTYPE
                info.mode = 0o755
                tar.addfile(info)
            else:
                info.size = os.path.getsize(path)
                info.mode = 0o755 if is_exec else 0o644
                with open(path, "rb") as f:
                    tar.addfile(info, f)
    with open(out_path, "wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0, compresslevel=9) as gz:
            gz.write(buf.getvalue())


def read_archive(path):
    """{rel below files/: size or None} of a files/ archive, plus problems with its entries."""
    entries, problems = {}, []
    with tarfile.open(path, "r:gz") as tar:
        for m in tar.getmembers():
            name = m.name.rstrip("/")
            if name == "files":
                continue
            if not name.startswith("files/"):
                problems.append("%s: outside files/" % m.name)
                continue
            rel = name[len("files/"):]
            if m.isdir():
                entries[rel] = None
            elif m.isfile():
                entries[rel] = m.size
            else:
                problems.append("%s: not a regular file or directory" % m.name)
    return entries, problems + check_tree(entries)
