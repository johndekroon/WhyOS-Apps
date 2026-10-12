#!/usr/bin/env python3
"""Pack the apps and write the published source into dist/v1/ (docs/repository.md §5).

    tools/publish.py [--base-url URL] [--out DIR] [<id> ...]

Publishing is a git commit of dist/: run this, commit apps/ and dist/
together, push. The badge reads dist/v1/ over HTTPS from the base URL
(default: raw.githubusercontent.com of this repository's main branch).

Per app: the checks of validate.py, then the package dist/v1/packages/
<id>-<version>.wpk (manifest.json first, then files/), the details
apps/<id>.json with every published version, the licence texts, the
source archive (when apps/<id>/source/ exists) and the icon and
screenshots as QOI. Then index.json and index.json.gz.

A published version never changes: a package that would come out
different for the same version is refused (raise version and build).
Without changes to apps/ the output is the same, serial and generated
included, so running it twice changes nothing.
"""
import argparse
import datetime
import gzip
import hashlib
import io
import json
import os
import sys
import tarfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pkglib  # noqa: E402
import validate  # noqa: E402

DEFAULT_BASE = "https://raw.githubusercontent.com/johndekroon/WhyOS-Apps/main/dist/v1/"
SOURCE_ID = "whyos"
ICON_SIZES = ((96, ""), (192, "@2"))
SCREEN_SIZE = 360

# fields of the manifest that only the app page needs (repository.md §5.2)
DETAIL_FIELDS = ("name", "summary", "description", "author", "maintainer", "homepage", "source",
                 "license", "upstream_version", "content_warning", "type", "category", "tags",
                 "permissions", "storage", "requires", "commands", "install_location")


def now():
    return datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def dump(obj):
    return json.dumps(obj, indent=1, sort_keys=True, ensure_ascii=False).encode() + b"\n"


def write_if_changed(path, data):
    """Write data unless the file already holds it. True when it changed."""
    try:
        with open(path, "rb") as f:
            if f.read() == data:
                return False
    except FileNotFoundError:
        pass
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + ".new", "wb") as f:
        f.write(data)
    os.replace(path + ".new", path)
    return True


def load(path):
    try:
        return pkglib.load_json(path)
    except FileNotFoundError:
        return None


def qoi_of(png, size):
    from PIL import Image  # only needed when an app has images
    import qoi
    im = Image.open(png).convert("RGBA")
    if im.size != (size, size):
        im = im.resize((size, size), Image.LANCZOS)
    return qoi.encode(size, size, im.tobytes())


def source_archive(src_dir, prefix):
    """Reproducible tar.gz of an app's source/ directory."""
    tree, problems = pkglib.collect([src_dir], lambda rel: False)
    if problems:
        sys.exit("\n".join(problems))
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        for rel in sorted(tree):
            path, is_exec = tree[rel]
            info = tarfile.TarInfo(prefix + "/" + rel)
            info.mtime, info.uid, info.gid, info.uname, info.gname = 0, 0, 0, "", ""
            if path is None:
                info.type, info.mode = tarfile.DIRTYPE, 0o755
                tar.addfile(info)
            else:
                info.size = os.path.getsize(path)
                info.mode = 0o755 if is_exec else 0o644
                with open(path, "rb") as f:
                    tar.addfile(info, f)
    out = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=out, mtime=0, compresslevel=9) as gz:
        gz.write(buf.getvalue())
    return out.getvalue()


def publish_app(app_id, out, base, categories, schema):
    """Publish one app; returns its index entry. Exits on a problem."""
    d = os.path.join(pkglib.ROOT, "apps", app_id)
    problems = validate.check_app(app_id, schema, categories)
    if problems:
        sys.exit("%s:\n    %s" % (app_id, "\n    ".join(problems)))
    m = pkglib.load_toml(os.path.join(d, "manifest.toml"))
    details_path = os.path.join(out, "apps", app_id + ".json")
    prev = load(details_path) or {}
    versions = [v for v in prev.get("versions", []) if v["version"] != m.get("version")]

    if "withdrawn" in m:
        details = dict(prev, id=app_id, name=m.get("name", prev.get("name", app_id)),
                       withdrawn=m["withdrawn"], format=1)
        data = dump(details)
        write_if_changed(details_path, data)
        return {"id": app_id, "name": details["name"], "withdrawn": m["withdrawn"]}

    same = [v for v in prev.get("versions", []) if v["version"] == m["version"]]
    if same and same[0]["build"] != m["build"]:
        sys.exit("%s %s was published as build %d: give a new version a new number"
                 % (app_id, m["version"], same[0]["build"]))
    if not same and versions and m["build"] <= max(v["build"] for v in versions):
        sys.exit("%s: build %d is not higher than the published %d"
                 % (app_id, m["build"], max(v["build"] for v in versions)))
    published = same[0]["published"] if same else now()

    art = pkglib.load_toml(os.path.join(d, "artifact.toml"))
    files, dirs = pkglib.files_of_archive(os.path.join(d, art["path"]))
    manifest = pkglib.manifest_json(m, categories, files, published)
    name = "%s-%s.wpk" % (app_id, m["version"])
    pkg_path = os.path.join(out, "packages", name)
    tmp = pkg_path + ".new"
    os.makedirs(os.path.dirname(pkg_path), exist_ok=True)
    pkglib.write_package(manifest, files, dirs, tmp)
    sha = pkglib.sha256_file(tmp)
    if os.path.exists(pkg_path) and pkglib.sha256_file(pkg_path) != sha:
        os.unlink(tmp)
        sys.exit("%s %s is published and its package would change: raise version and build"
                 % (app_id, m["version"]))
    os.replace(tmp, pkg_path)
    package = {"url": base + "packages/" + name, "sha256": sha,
               "size": os.path.getsize(pkg_path), "installed_size": manifest["size"]["installed"]}

    # licence texts, next to the details (repository.md §5.2)
    lic = {}
    for rel, size, digest, data in files:
        if rel.startswith("LICENSES/"):
            f = rel[len("LICENSES/"):]
            write_if_changed(os.path.join(out, "licenses", app_id, f), data)
            lic[f] = "licenses/%s/%s" % (app_id, f)
    bundled = []
    for b in m.get("bundled", []):
        b = dict(b)
        b["license_url"] = lic.get(b["license_file"])
        bundled.append(b)

    source_url = None
    if os.path.isdir(os.path.join(d, "source")):
        sname = "%s-%s-source.tar.gz" % (app_id, m["version"])
        write_if_changed(os.path.join(out, "sources", sname),
                         source_archive(os.path.join(d, "source"), "%s-%s-source" % (app_id, m["version"])))
        source_url = base + "sources/" + sname

    icon = None
    if os.path.isfile(os.path.join(d, "icon.png")):
        for size, suffix in ICON_SIZES:
            write_if_changed(os.path.join(out, "icons", app_id + suffix + ".qoi"),
                             qoi_of(os.path.join(d, "icon.png"), size))
        icon = "icons/%s.qoi" % app_id
    screens = []
    sdir = os.path.join(d, "screenshots")
    for n in range(1, 7):
        png = os.path.join(sdir, "%d.png" % n)
        if not os.path.isfile(png):
            break
        rel = "screens/%s-%d.qoi" % (app_id, n)
        write_if_changed(os.path.join(out, rel), qoi_of(png, SCREEN_SIZE))
        screens.append(rel)

    versions.append({"version": m["version"], "build": m["build"], "published": published,
                     "changelog": m.get("changelog", ""), "package": package,
                     "source_archive": source_url})
    versions.sort(key=lambda v: -v["build"])
    details = {k: manifest[k] for k in DETAIL_FIELDS if k in manifest}
    details.update(format=1, id=app_id, launch=manifest["launch"], bundled=bundled,
                   license_url=lic.get(m.get("license_file", app_id + ".txt")),
                   screenshots=screens, icon=icon, versions=versions)
    data = dump(details)
    write_if_changed(details_path, data)

    entry = {
        "id": app_id, "name": m["name"], "summary": m["summary"], "type": m["type"],
        "category": m["category"], "tags": m.get("tags", []), "author": m["author"],
        "terminal": manifest["launch"]["terminal"] is not False or
        (manifest["launch"]["menu"] == "none" and bool(m.get("commands"))),
        "commands": [c["name"] for c in m.get("commands", [])],
        "updated": published[:10],
        "details_sha256": hashlib.sha256(data).hexdigest(),
        "requires": m["requires"],
        "install_location": manifest["install_location"],
        "latest": {"version": m["version"], "build": m["build"], "package": package},
    }
    if icon:
        entry["icon"] = icon
    if "launch" in m and m["launch"].get("icon"):
        entry["menu_icon"] = m["launch"]["icon"]
    return entry


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base-url", default=DEFAULT_BASE)
    ap.add_argument("--out", default=os.path.join(pkglib.ROOT, "dist", "v1"))
    ap.add_argument("ids", nargs="*")
    a = ap.parse_args()
    base = a.base_url if a.base_url.endswith("/") else a.base_url + "/"
    out = a.out

    categories_path = os.path.join(pkglib.ROOT, "categories.json")
    categories = pkglib.load_json(categories_path)
    schema = (pkglib.load_json(os.path.join(pkglib.ROOT, "schema", "manifest.schema.json"))
              if validate.jsonschema else None)
    if schema is None:
        print("note: jsonschema not installed, schema checks skipped")

    apps_dir = os.path.join(pkglib.ROOT, "apps")
    all_ids = sorted(i for i in os.listdir(apps_dir)
                     if os.path.isfile(os.path.join(apps_dir, i, "manifest.toml")))
    ids = a.ids or all_ids
    prev = load(os.path.join(out, "index.json")) or {}
    entries = {e["id"]: e for e in prev.get("apps", [])}
    for app_id in ids:
        entries[app_id] = publish_app(app_id, out, base, categories, schema)
        print("%-10s %s" % (app_id, entries[app_id].get("latest", {}).get("version", "withdrawn")))

    cat_data = open(categories_path, "rb").read()
    write_if_changed(os.path.join(out, "categories.json"), cat_data)
    index = {"format": 1, "source": SOURCE_ID, "base_url": base,
             "categories_sha256": hashlib.sha256(cat_data).hexdigest(),
             "apps": [entries[i] for i in sorted(entries)]}
    featured = os.path.join(pkglib.ROOT, "featured.json")
    if os.path.isfile(featured):
        index["featured"] = pkglib.load_json(featured)
    same = {k: v for k, v in prev.items() if k not in ("serial", "generated")} == index
    index["serial"] = prev.get("serial", 0) + (0 if same else 1)
    index["generated"] = prev.get("generated") if same else now()
    data = dump(index)
    if write_if_changed(os.path.join(out, "index.json"), data):
        gz = io.BytesIO()
        with gzip.GzipFile(filename="", mode="wb", fileobj=gz, mtime=0, compresslevel=9) as f:
            f.write(data)
        write_if_changed(os.path.join(out, "index.json.gz"), gz.getvalue())
        print("index: serial %d, %d apps" % (index["serial"], len(index["apps"])))
    else:
        print("index: unchanged (serial %d)" % index["serial"])


if __name__ == "__main__":
    main()
