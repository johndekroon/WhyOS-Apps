"""Tests of the host tools: pack, publish, the package checks, QOI.

    python3 -m pytest tests/       (needs jsonschema and Pillow for all of them)
"""
import json
import os
import shutil
import subprocess
import sys
import tarfile

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import pkglib  # noqa: E402
import qoi  # noqa: E402


def run(*args, root=ROOT, check=True):
    env = dict(os.environ, WHYOS_APPS_ROOT=root)
    r = subprocess.run([sys.executable, *args], env=env, capture_output=True, text=True)
    if check:
        assert r.returncode == 0, r.stdout + r.stderr
    return r


@pytest.fixture
def repo(tmp_path):
    """A copy of this repository with one small app (gnuboy)."""
    root = tmp_path / "repo"
    for f in ("categories.json",):
        os.makedirs(root, exist_ok=True)
        shutil.copy(os.path.join(ROOT, f), root / f)
    shutil.copytree(os.path.join(ROOT, "schema"), root / "schema")
    shutil.copytree(os.path.join(ROOT, "apps", "gnuboy"), root / "apps" / "gnuboy")
    return str(root)


def publish(repo, *ids):
    out = os.path.join(repo, "dist", "v1")
    return run(os.path.join(ROOT, "tools", "publish.py"), "--out", out, *ids, root=repo), out


def test_qoi_round_trip():
    import random
    rnd = random.Random(1)
    px = bytearray()
    for i in range(37 * 23):
        px += bytes((rnd.choice((0, 7, 255)), i % 256, (i * 3) % 256, rnd.choice((255, 128))))
    w, h, back = qoi.decode(qoi.encode(37, 23, bytes(px)))
    assert (w, h, back) == (37, 23, bytes(px))


def test_publish_is_idempotent(repo):
    _, out = publish(repo)
    before = {f: open(os.path.join(dp, f), "rb").read() for dp, _, fs in os.walk(out) for f in fs}
    r, _ = publish(repo)
    after = {f: open(os.path.join(dp, f), "rb").read() for dp, _, fs in os.walk(out) for f in fs}
    assert before == after
    assert "unchanged (serial 1)" in r.stdout


def test_published_package_passes_checks_and_schema(repo):
    _, out = publish(repo)
    index = json.load(open(os.path.join(out, "index.json")))
    entry = index["apps"][0]
    pkg = os.path.join(out, "packages", "gnuboy-1.0.0.wpk")
    assert pkglib.check_package(pkg, entry["latest"]["package"]["sha256"]) == []
    with tarfile.open(pkg, "r:gz") as tar:
        names = tar.getnames()
        m = json.load(tar.extractfile("manifest.json"))
    assert names[0] == "manifest.json"
    assert m["launch"]["menu"] == "games" and m["launch"]["rotation"] == "auto"
    assert m["size"]["installed"] == entry["latest"]["package"]["installed_size"]
    jsonschema = pytest.importorskip("jsonschema")
    jsonschema.validate(m, json.load(open(os.path.join(ROOT, "schema", "manifest.schema.json"))))
    jsonschema.validate(index, json.load(open(os.path.join(ROOT, "schema", "index.schema.json"))))
    details = json.load(open(os.path.join(out, "apps", "gnuboy.json")))
    assert details["versions"][0]["package"]["sha256"] == entry["latest"]["package"]["sha256"]
    for b in details["bundled"]:
        assert os.path.isfile(os.path.join(out, b["license_url"]))


def edit_manifest(repo, old, new):
    p = os.path.join(repo, "apps", "gnuboy", "manifest.toml")
    s = open(p).read()
    assert old in s
    open(p, "w").write(s.replace(old, new, 1))


def test_published_version_does_not_change(repo):
    publish(repo)
    edit_manifest(repo, 'summary = "Game Boy and Game Boy Color emulator"', 'summary = "Changed"')
    r = run(os.path.join(ROOT, "tools", "publish.py"), "--out", os.path.join(repo, "dist", "v1"),
            root=repo, check=False)
    assert r.returncode != 0 and "raise version and build" in r.stderr


def test_new_version_keeps_the_old_one(repo):
    _, out = publish(repo)
    edit_manifest(repo, 'version = "1.0.0"\nbuild = 1', 'version = "1.0.1"\nbuild = 2')
    r, _ = publish(repo)
    assert "serial 2" in r.stdout
    details = json.load(open(os.path.join(out, "apps", "gnuboy.json")))
    assert [v["build"] for v in details["versions"]] == [2, 1]
    assert os.path.isfile(os.path.join(out, "packages", "gnuboy-1.0.0.wpk"))
    assert os.path.isfile(os.path.join(out, "packages", "gnuboy-1.0.1.wpk"))


def test_build_must_rise(repo):
    publish(repo)
    edit_manifest(repo, 'version = "1.0.0"', 'version = "1.0.1"')
    r = run(os.path.join(ROOT, "tools", "publish.py"), "--out", os.path.join(repo, "dist", "v1"),
            root=repo, check=False)
    assert r.returncode != 0 and "not higher" in r.stderr


def test_withdrawn_app_stays_in_the_index(repo):
    _, out = publish(repo)
    p = os.path.join(repo, "apps", "gnuboy", "manifest.toml")
    open(p, "w").write('format = 1\nid = "gnuboy"\nname = "Game Boy"\nwithdrawn = "Test"\n')
    publish(repo)
    entry = json.load(open(os.path.join(out, "index.json")))["apps"][0]
    assert entry["withdrawn"] == "Test" and "latest" not in entry


def test_bad_packages_are_refused(tmp_path):
    run(os.path.join(HERE, "make_bad_packages.py"), str(tmp_path))
    cases = json.load(open(tmp_path / "cases.json"))
    assert len(cases) > 15
    for name, verdict in cases.items():
        problems = pkglib.check_package(str(tmp_path / (name + ".wpk")))
        assert (problems == []) == (verdict == "accept"), (name, problems)


def test_category_tree_stays_small():
    """At most four visible categories per type, and every retired id
    (alias) maps to exactly one category (repository.md §3)."""
    tree = pkglib.load_json(os.path.join(ROOT, "categories.json"))
    for t in tree["types"]:
        cats = [c for c in t["categories"] if not c.get("hidden")]
        assert t.get("hidden") or len(cats) <= 4, t["id"]
        ids = [c["id"] for c in t["categories"]] + [a for c in t["categories"] for a in c.get("aliases", [])]
        assert len(ids) == len(set(ids)), t["id"]


def test_alias_category_is_published_as_its_new_id(repo):
    m = os.path.join(repo, "apps", "gnuboy", "manifest.toml")
    with open(m) as f:
        s = f.read()
    with open(m, "w") as f:
        f.write(s.replace('category = "emulators"', 'category = "arcade"'))
    _, out = publish(repo)
    with open(os.path.join(out, "index.json")) as f:
        entry = [a for a in json.load(f)["apps"] if a["id"] == "gnuboy"][0]
    assert entry["category"] == "action"
