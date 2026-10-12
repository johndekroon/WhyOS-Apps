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

# the repository; tests point it at a copy
ROOT = os.environ.get("WHYOS_APPS_ROOT") or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

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


# ---------------------------------------------------------------------------
# Packing (docs/package.md §1, §2.9) and publishing (docs/repository.md §5)

INFO_MAX = 32


def files_of_archive(path):
    """[(rel, size, sha256, data)] of the regular files in a files/ archive, plus
    [rel] of its directories, in archive order. The archive must already pass
    read_archive()."""
    files, dirs = [], []
    with tarfile.open(path, "r:gz") as tar:
        for m in tar.getmembers():
            name = m.name.rstrip("/")
            if name == "files":
                continue
            rel = name[len("files/"):]
            if m.isdir():
                dirs.append(rel)
            elif m.isfile():
                data = tar.extractfile(m).read()
                files.append((rel, len(data), hashlib.sha256(data).hexdigest(), data))
    return files, dirs


def category_of(categories, type_id, category_id):
    for t in categories["types"]:
        if t["id"] == type_id:
            for c in t["categories"]:
                if c["id"] == category_id:
                    return t, c
    return None, None


def short_info(summary):
    """launch.info default: the summary cut to INFO_MAX at a word."""
    if len(summary) <= INFO_MAX:
        return summary
    cut = summary[:INFO_MAX - 1].rsplit(" ", 1)[0].rstrip(",;:")
    return cut + "…"


def manifest_json(m, categories, files, published):
    """The manifest as shipped (docs/package.md §2.9): manifest.toml plus the
    defaults a client would otherwise have to know, plus generated fields.
    files: [(rel, size, sha256, data)]. size.download is not in it (the
    package cannot hold its own size); the index and details have it."""
    out = json.loads(json.dumps(m))   # deep copy, TOML types are JSON types here
    launch = out.setdefault("launch", {})
    t, c = category_of(categories, m["type"], m["category"])
    launch.setdefault("info", short_info(m["summary"]))
    launch.setdefault("terminal", False)
    launch.setdefault("rotation", "auto")
    launch.setdefault("menu", (c or {}).get("menu") or (t or {}).get("menu") or "apps")
    out.setdefault("install_location", "sd")
    st = out.get("storage", {})
    if "config" in st:
        st["config"].setdefault("max_kb", 16)
    out["published"] = published
    out["size"] = {"installed": sum(f[1] for f in files)}
    out["files"] = {"sha256": {f[0]: f[2] for f in files}}
    return out


def _tarinfo(name, size=None):
    info = tarfile.TarInfo(name)
    info.mtime = 0
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    if size is None:
        info.type = tarfile.DIRTYPE
        info.mode = 0o755
    else:
        info.size = size
        info.mode = 0o644
    return info


def write_package(manifest, files, dirs, out_path):
    """A .wpk: gzip'd ustar with manifest.json first, then files/ (package.md §1.1).
    Reproducible: fixed order, times and owners."""
    body = json.dumps(manifest, indent=1, sort_keys=True, ensure_ascii=False).encode() + b"\n"
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        tar.addfile(_tarinfo("manifest.json", len(body)), io.BytesIO(body))
        tar.addfile(_tarinfo("files"))
        entries = [(d, None, None) for d in dirs] + [(f[0], f[1], f[3]) for f in files]
        for rel, size, data in sorted(entries, key=lambda e: e[0]):
            if size is None:
                tar.addfile(_tarinfo("files/" + rel))
            else:
                tar.addfile(_tarinfo("files/" + rel, size), io.BytesIO(data))
    with open(out_path, "wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0, compresslevel=9) as gz:
            gz.write(buf.getvalue())


def check_package(path, expect_sha256=None):
    """The checks a client makes on a .wpk (package.md §1): problems, or [].
    manifest.json first and well-formed; only manifest.json and files/ (others
    reserved, ignored); entry rules; every file's size and SHA-256 as in the
    manifest; no file the manifest does not list."""
    problems = []
    if expect_sha256 and sha256_file(path) != expect_sha256:
        return ["SHA-256 of the package does not match the index"]
    try:
        tar = tarfile.open(path, "r:gz")
        members = tar.getmembers()
    except (tarfile.TarError, OSError, EOFError) as e:
        return ["not a gzip'd tar archive: %s" % e]
    with tar:
        if not members or members[0].name != "manifest.json" or not members[0].isfile():
            return ["manifest.json is not the first entry"]
        try:
            m = json.loads(tar.extractfile(members[0]).read())
        except ValueError as e:
            return ["manifest.json: %s" % e]
        want = m.get("files", {}).get("sha256", {})
        entries, seen = {}, set()
        for mem in members[1:]:
            name = mem.name.rstrip("/")
            if name == "files" and mem.isdir():
                continue
            if not name.startswith("files/"):
                if name.startswith("/") or ".." in name.split("/"):
                    problems.append("%s: absolute path or '..'" % mem.name)
                continue        # reserved for later formats: ignored
            rel = name[len("files/"):]
            if mem.isdir():
                entries[rel] = None
            elif mem.isfile():
                entries[rel] = mem.size
                if rel not in want:
                    problems.append("%s: not in the manifest's files.sha256" % rel)
                else:
                    data = tar.extractfile(mem).read()
                    if hashlib.sha256(data).hexdigest() != want[rel]:
                        problems.append("%s: SHA-256 differs from the manifest" % rel)
                    seen.add(rel)
            else:
                problems.append("%s: not a regular file or directory" % mem.name)
        problems += check_tree(entries)
        for rel in sorted(set(want) - seen):
            problems.append("%s: in the manifest but not in the package" % rel)
    return problems
