#!/usr/bin/env python3
"""Manifest format 2: metadata.json, in the format of the badge.team app repository, with the
WhyOS fields beside it (docs/metadata.md).

    tools/metadata.py convert <id> ...     apps/<id>/manifest.toml (format 1) -> apps/<id>/metadata.json
    tools/metadata.py export <id> ...      print the app as the app repository takes it: their fields only
    tools/metadata.py check <file> ...     check metadata.json files against the app repository's rules

The badge keeps reading format 1: tools/publish.py turns format 2 into the format-1 manifest.json
in the package and the index, so clients need no change.
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pkglib  # noqa: E402

# the app repository's rules (its .validator/schema.json and validate.js)
NAME_DESC = re.compile(r"^[A-Za-z0-9_\- .!@#$%^&*()/\\]+$")
TOP = {"name", "description", "repository_url", "categories", "version", "icon", "author", "license_type",
       "license_file", "application"}
TOP_REQUIRED = {"name", "description", "categories", "version", "icon", "author", "license_type", "application"}
APPLICATION = {"targets", "type", "revision", "bsp_version", "executable", "interpreter", "assets",
               "external_only", "external_preferred", "internal_only", "internal_preferred"}

# what a WhyOS package is in their terms: a device name and an executable type of its own, so a
# launcher for the badge's original firmware (target "why2025", type "appfs") never takes it
TARGET = "why2025-whyos"
TYPE = "whyos"
ICON_SIZES = ("16x16", "32x32", "64x64")

# our type/category -> theirs (free strings; these are the ones in use there)
CATEGORIES = {
    ("app", "internet"): ["communication"],
    ("app", "hardware"): ["hardware"],
    ("app", "tools"): ["tools"],
    ("app", "other"): ["tools"],
    ("game", "action"): ["games"],
    ("game", "puzzle"): ["games"],
    ("game", "emulators"): ["games", "emulators"],
    ("game", "other"): ["games"],
    ("system", "store"): ["system"],
    ("system", "data"): ["system"],
    ("system", "other"): ["system"],
}

# format-1 fields that have a place among theirs (the rest go into "whyos")
MAPPED = {"format", "name", "version", "build", "author", "license", "license_file", "homepage",
          "install_location"}


def plain(text, limit=256):
    """Text in the characters their name and description allow: a colon or semicolon becomes a
    dash, other punctuation goes, accents lose their marks."""
    import unicodedata
    text = unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode()
    for a, b in ((": ", " - "), (":", " -"), ("; ", " - "), (" + ", " and "), ("+", " plus "),
                 ("=", " is "), ("×", "x")):
        text = text.replace(a, b)
    text = re.sub(r"[^A-Za-z0-9_\- .!@#$%^&*()/\\]", "", text.replace("\n", " "))
    text = re.sub(r" +", " ", text).strip()
    return text[:limit].rstrip()


def categories_of(m, categories):
    t, c = m.get("type", "app"), m.get("category", "other")
    found = pkglib.category_of(categories, t, c)[1] if categories else None
    cid = found["id"] if found else c
    return CATEGORIES.get((t, cid), ["system" if t == "system" else "games" if t == "game" else "tools"])


def from_manifest(m, categories=None, icons=None):
    """A format-1 manifest (manifest.toml as read) as format 2 (metadata.json)."""
    whyos = {k: v for k, v in m.items() if k not in MAPPED}
    whyos["format"] = 2
    launch = dict(whyos.pop("launch", {}))
    app = {"targets": [TARGET], "type": TYPE}
    if "build" in m:
        app["revision"] = m["build"]
    if launch.get("exec"):
        app["executable"] = launch.pop("exec")
    if launch:
        whyos["launch"] = launch
    if m.get("install_location") == "flash":
        app["internal_only"] = True
    else:
        app["external_only"] = True
    app["assets"] = []
    meta = {
        "name": m["name"],
        "description": plain(m.get("summary", m["name"])),
        "repository_url": m.get("homepage") or m.get("source", ""),
        "categories": categories_of(m, categories),
        "version": m.get("version", "0"),
        "icon": icons or {s: "icon%s.png" % s.split("x")[0] for s in ICON_SIZES},
        "author": m.get("author", ""),
        "license_type": m.get("license", "proprietary"),
        "license_file": m.get("license_file", m["id"] + ".txt"),
        "application": [app],
        "whyos": whyos,
    }
    if not meta["repository_url"]:
        del meta["repository_url"]
    return meta


def whyos_application(meta):
    for a in meta.get("application", []):
        if a.get("type") == TYPE and TARGET in a.get("targets", []):
            return a
    return None


def to_manifest(meta, app_id=None):
    """A format-2 metadata.json as the format-1 manifest the tools and the badge use."""
    w = dict(meta.get("whyos", {}))
    app = whyos_application(meta) or {}
    m = {"format": 1}
    m.update({k: v for k, v in w.items() if k != "format"})
    m.setdefault("id", app_id)
    m["name"] = meta.get("name")
    if "withdrawn" in w:        # a tombstone: its id and name (their required fields are filler)
        return m
    m["version"] = meta.get("version")
    if "revision" in app:
        m["build"] = app["revision"]
    m["author"] = meta.get("author")
    m["license"] = meta.get("license_type")
    if meta.get("license_file"):
        m["license_file"] = meta["license_file"]
    m.setdefault("summary", meta.get("description"))
    if meta.get("repository_url") and meta["repository_url"] != w.get("source"):
        m["homepage"] = meta["repository_url"]
    m["install_location"] = "flash" if app.get("internal_only") else "sd"
    launch = dict(w.get("launch", {}))
    if app.get("executable"):
        launch = dict(exec=app["executable"], **launch)
    if launch:
        m["launch"] = launch
    return {k: v for k, v in m.items() if v is not None}


def export(meta):
    """Only the app repository's fields: what their validator takes (check() finds nothing)."""
    out = {k: v for k, v in meta.items() if k in TOP}
    out["application"] = [{k: v for k, v in a.items() if k in APPLICATION} for a in meta.get("application", [])]
    out["name"] = plain(out.get("name", ""), 128)
    out["description"] = plain(out.get("description", ""))
    return out


def check(meta, folder=None):
    """The app repository's rules for one metadata.json (their schema, then that every asset's
    source_file is in the app's folder); with folder also the icons and the executable, which
    their launcher downloads. A list of problems."""
    p = []
    for k in sorted(set(meta) - TOP):
        p.append("%s: not a field of the app repository" % k)
    for k in sorted(TOP_REQUIRED - set(meta)):
        p.append("%s: missing" % k)
    for k, lo, hi in (("name", 3, 128), ("description", 0, 256)):
        v = meta.get(k)
        if not isinstance(v, str):
            continue
        if not lo <= len(v) <= hi:
            p.append("%s: %d characters, not %d to %d" % (k, len(v), lo, hi))
        if v and not NAME_DESC.match(v):
            bad = sorted(set(c for c in v if not NAME_DESC.match(c)))
            p.append("%s: characters they do not allow: %s" % (k, " ".join(map(repr, bad))))
    for k in ("repository_url", "version", "author", "license_type", "license_file"):
        if k in meta and not isinstance(meta[k], str):
            p.append("%s: not a string" % k)
    if not isinstance(meta.get("categories", []), list) or not all(isinstance(c, str) for c in meta.get("categories", [])):
        p.append("categories: not a list of strings")
    if not isinstance(meta.get("icon", {}), dict) or ("icon" in meta and not meta["icon"]):
        p.append("icon: an object with at least one size")
    apps = meta.get("application", [])
    if not isinstance(apps, list) or not apps:
        p.append("application: at least one")
        apps = []
    for i, a in enumerate(apps):
        for k in sorted(set(a) - APPLICATION):
            p.append("application[%d].%s: not a field of the app repository" % (i, k))
        for k in ("targets", "type"):
            if k not in a:
                p.append("application[%d].%s: missing" % (i, k))
        for k in ("revision", "bsp_version"):
            if k in a and not isinstance(a[k], int):
                p.append("application[%d].%s: not an integer" % (i, k))
        for asset in a.get("assets", []):
            if set(asset) != {"source_file", "target_file"}:
                p.append("application[%d].assets: source_file and target_file, nothing else" % i)
            elif folder and not os.path.isfile(os.path.join(folder, asset["source_file"])):
                p.append("application[%d].assets: %s is not in the folder" % (i, asset["source_file"]))
        if folder and a.get("executable") and a.get("type") in ("appfs", "plugin") and \
                not os.path.isfile(os.path.join(folder, a["executable"])):
            p.append("application[%d].executable: %s is not in the folder" % (i, a["executable"]))
    if folder:
        for size, f in (meta.get("icon") or {}).items():
            if not os.path.isfile(os.path.join(folder, f)):
                p.append("icon %s: %s is not in the folder" % (size, f))
    return p


def check_format2(meta, app_id):
    """What format 2 adds to their rules: one WhyOS application, the WhyOS fields."""
    p = check({k: v for k, v in meta.items() if k != "whyos"})
    w = meta.get("whyos")
    if not isinstance(w, dict) or w.get("format") != 2:
        p.append("whyos: an object with format = 2")
        return p
    if "withdrawn" not in w and whyos_application(meta) is None:
        p.append('application: none with targets ["%s"] and type "%s"' % (TARGET, TYPE))
    if w.get("id", app_id) != app_id:
        p.append("whyos.id %r is not the directory name" % w.get("id"))
    for k in sorted(MAPPED & set(w) - {"format"}):
        p.append("whyos.%s: belongs at the top (or in the application) in format 2" % k)
    return p


def same(m):
    """A format-1 manifest with what does not matter evened out: the licence file's default, a
    homepage that is the source."""
    m = dict(m)
    m.setdefault("license_file", "%s.txt" % m.get("id"))
    m.setdefault("install_location", "sd")
    if m.get("homepage") and m.get("homepage") == m.get("source"):
        del m["homepage"]
    return m


def write(path, meta):
    data = json.dumps(meta, indent=2, ensure_ascii=False) + "\n"
    with open(path + ".new", "w") as f:
        f.write(data)
    os.replace(path + ".new", path)


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("convert", "export", "check"):
        sys.exit(__doc__)
    cmd, args = sys.argv[1], sys.argv[2:]
    categories = pkglib.load_json(os.path.join(pkglib.ROOT, "categories.json"))
    bad = 0
    for a in args:
        if cmd == "check":
            p = check(pkglib.load_json(a), os.path.dirname(os.path.abspath(a)))
            print("%s: %s" % (a, "OK" if not p else "; ".join(p)))
            bad += bool(p)
            continue
        d = os.path.join(pkglib.ROOT, "apps", a)
        if cmd == "convert":
            m = pkglib.load_toml(os.path.join(d, "manifest.toml"))
            meta = from_manifest(m, categories)
            if same(to_manifest(meta, a)) != same(m):
                sys.exit("%s: the conversion would lose something; not written" % a)
            write(os.path.join(d, "metadata.json"), meta)
            os.unlink(os.path.join(d, "manifest.toml"))
            print("%s: metadata.json (format 2), manifest.toml removed" % a)
        else:
            print(json.dumps(export(pkglib.load_metadata(d, categories)), indent=2, ensure_ascii=False))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
