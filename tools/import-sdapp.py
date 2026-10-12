#!/usr/bin/env python3
"""Turn an app built by why2025-linux sdapps/build.sh into apps/<id>/files.tar.gz.

    tools/import-sdapp.py <id> <built app dir> --built-from "<what produced it>"

The built directory is what build.sh leaves in ~/br-badge/sdapps/badge/apps/<id>/
(copy it out of the VM first). app.conf and *.gdb are dropped (the store writes
app.conf from the manifest), apps/<id>/overlay/ is added on top, and
apps/<id>/licenses/ becomes LICENSES/. The archive is reproducible; its SHA-256
goes into apps/<id>/artifact.toml (docs/repository.md §1.1).
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pkglib  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("id")
    ap.add_argument("built")
    ap.add_argument("--built-from", required=True)
    a = ap.parse_args()

    app = os.path.join(pkglib.ROOT, "apps", a.id)
    if not os.path.isfile(os.path.join(app, "manifest.toml")):
        sys.exit("no apps/%s/manifest.toml" % a.id)

    skip = lambda rel: rel == "app.conf" or rel.endswith(".gdb")  # noqa: E731
    tree, problems = pkglib.collect([a.built, os.path.join(app, "overlay")], skip)
    lic, p2 = pkglib.collect([os.path.join(app, "licenses")], lambda rel: False)
    problems += p2
    tree["LICENSES"] = (None, False)
    for rel, (path, _) in lic.items():
        tree["LICENSES/" + rel] = (path, False)
    problems += pkglib.check_tree({r: (os.path.getsize(p) if p else None) for r, (p, _) in tree.items()})
    if problems:
        sys.exit("\n".join(problems))

    out = os.path.join(app, "files.tar.gz")
    pkglib.write_archive(tree, out)
    digest = pkglib.sha256_file(out)
    with open(os.path.join(app, "artifact.toml"), "w") as f:
        f.write('# The prebuilt files/ archive (docs/repository.md §1.1), written by tools/import-sdapp.py\n')
        f.write('path = "files.tar.gz"\n')
        f.write('sha256 = "%s"\n' % digest)
        f.write('built_from = "%s"\n' % a.built_from.replace('"', "'"))
    files = sum(1 for p, _ in tree.values() if p)
    print("%s: %d files, %d bytes compressed, sha256 %s" % (a.id, files, os.path.getsize(out), digest))


if __name__ == "__main__":
    main()
