#!/usr/bin/env python3
"""Pack one app into <out>/<id>-<version>.wpk without publishing it (docs/package.md §1).

    tools/pack.py <id> [<out dir>]        default out: .

For trying a package on a badge by hand; tools/publish.py packs for the store.
"""
import datetime
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pkglib  # noqa: E402
import validate  # noqa: E402


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    app_id, out = sys.argv[1], sys.argv[2] if len(sys.argv) == 3 else "."
    categories = pkglib.load_json(os.path.join(pkglib.ROOT, "categories.json"))
    problems = validate.check_app(app_id, None, categories)
    if problems:
        sys.exit("\n".join(problems))
    d = os.path.join(pkglib.ROOT, "apps", app_id)
    m = pkglib.load_toml(os.path.join(d, "manifest.toml"))
    art = pkglib.load_toml(os.path.join(d, "artifact.toml"))
    files, dirs = pkglib.files_of_archive(os.path.join(d, art["path"]))
    stamp = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    manifest = pkglib.manifest_json(m, categories, files, stamp)
    path = os.path.join(out, "%s-%s.wpk" % (app_id, m["version"]))
    pkglib.write_package(manifest, files, dirs, path)
    print("%s: %d bytes, sha256 %s" % (path, os.path.getsize(path), pkglib.sha256_file(path)))


if __name__ == "__main__":
    main()
