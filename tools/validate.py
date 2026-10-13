#!/usr/bin/env python3
"""Check apps/*/ against the specification (docs/repository.md §4, steps 1-3 and 6).

    tools/validate.py [<id> ...]

Per app: the manifest against schema/manifest.schema.json (needs jsonschema)
and categories.json; the directory name; artifact.toml and the SHA-256 of
files.tar.gz; the entry rules for the archive; that launch.exec, command and
hook programs exist in it; that every licence file is in LICENSES/; and that
there is a place for the corresponding source. Exit status 1 on any problem.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pkglib  # noqa: E402

try:
    import jsonschema
except ImportError:
    jsonschema = None

PLACEHOLDER = re.compile(r"%[dDCTfuaA%]")


def check_app(app_id, schema, categories):
    d = os.path.join(pkglib.ROOT, "apps", app_id)
    p = []
    m = pkglib.load_toml(os.path.join(d, "manifest.toml"))

    if schema is not None:
        v = jsonschema.Draft202012Validator(schema)
        p += ["manifest: %s at %s" % (e.message, "/".join(map(str, e.path)) or "top") for e in v.iter_errors(m)]
    if m.get("id") != app_id:
        p.append("manifest id %r is not the directory name" % m.get("id"))
    if "withdrawn" in m:
        return p

    if pkglib.category_of(categories, m.get("type"), m.get("category"))[1] is None:
        p.append("category %r does not exist under type %r" % (m.get("category"), m.get("type")))

    art_path = os.path.join(d, "artifact.toml")
    if not os.path.isfile(art_path):
        if not os.path.isfile(os.path.join(d, "build.sh")):
            p.append("neither artifact.toml nor build.sh")
        return p
    art = pkglib.load_toml(art_path)
    if "path" not in art:
        return p + ["artifact.toml without path: url artifacts are checked by CI only"]
    archive = os.path.join(d, art["path"])
    if not os.path.isfile(archive):
        return p + ["artifact %s is missing" % art["path"]]
    if pkglib.sha256_file(archive) != art.get("sha256"):
        p.append("SHA-256 of %s does not match artifact.toml" % art["path"])

    entries, problems = pkglib.read_archive(archive)
    p += problems
    files = {r for r, s in entries.items() if s is not None}
    launch = m.get("launch", {})
    programs = [("launch.exec", launch.get("exec"))]
    programs += [("command %s" % c["name"], c["exec"]) for c in m.get("commands", [])]
    programs += [("hook %s" % k, v) for k, v in m.get("hooks", {}).items()]
    for what, prog in programs:
        if prog and prog not in files:
            p.append("%s: %s is not in the package" % (what, prog))

    lic_files = [m.get("license_file", app_id + ".txt")]
    lic_files += [b["license_file"] for b in m.get("bundled", [])]
    for lf in lic_files:
        if "LICENSES/" + lf not in files:
            p.append("licence file LICENSES/%s is not in the package" % lf)
    for lf in sorted(f for f in files if f.startswith("LICENSES/")):
        if lf[len("LICENSES/"):] not in lic_files:
            p.append("%s is not referenced by license_file or bundled" % lf)

    if not m.get("source"):
        p.append("no source: where is the corresponding source? (package.md §2.10)")
    elif "/apps/%s/source" % app_id in m["source"] and not os.path.isfile(os.path.join(d, "source", "SOURCES.md")):
        p.append("source points into this repository but source/SOURCES.md is missing")

    data = m.get("storage", {}).get("data", {})
    for uf in data.get("user_folders", []):
        if uf not in data.get("folders", []):
            p.append("user folder %s is not in storage.data.folders" % uf)
    for key in ("args", "files"):
        val = launch.get(key, "")
        if "%" in val.replace("%%", "") and not PLACEHOLDER.search(val):
            p.append("launch.%s: unknown placeholder" % key)
    return p


def main():
    apps_dir = os.path.join(pkglib.ROOT, "apps")
    ids = sys.argv[1:] or sorted(os.listdir(apps_dir))
    schema = pkglib.load_json(os.path.join(pkglib.ROOT, "schema", "manifest.schema.json")) if jsonschema else None
    if schema is None:
        print("note: jsonschema not installed, schema checks skipped")
    categories = pkglib.load_json(os.path.join(pkglib.ROOT, "categories.json"))
    bad = 0
    for app_id in ids:
        if not os.path.isfile(os.path.join(apps_dir, app_id, "manifest.toml")):
            continue
        problems = check_app(app_id, schema, categories)
        print("%-10s %s" % (app_id, "OK" if not problems else "%d problem(s)" % len(problems)))
        for prob in problems:
            print("    " + prob)
        bad += bool(problems)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
