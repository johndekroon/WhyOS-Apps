"""Manifest format 2 (metadata.json, docs/metadata.md): the conversion from format 1 and back, the
export in the app repository's format, and publishing an app written in format 2.

    python3 -m pytest tests/       (publishing needs Pillow, as in test_tools.py)
"""
import json
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)
import metadata  # noqa: E402
import pkglib  # noqa: E402
from test_tools import publish, repo, run  # noqa: E402,F401

CATEGORIES = pkglib.load_json(os.path.join(ROOT, "categories.json"))
APPS = sorted(a for a in os.listdir(os.path.join(ROOT, "apps")) if pkglib.manifest_path(os.path.join(ROOT, "apps", a)))


@pytest.mark.parametrize("app_id", APPS)
def test_every_app_goes_to_format_2_and_back_unchanged(app_id):
    d = os.path.join(ROOT, "apps", app_id)
    m = pkglib.load_manifest(d)
    meta = metadata.from_manifest(m, CATEGORIES)
    assert json.dumps(metadata.to_manifest(meta, app_id), sort_keys=True) == json.dumps(m, sort_keys=True)
    assert metadata.check_format2(meta, app_id) == []


@pytest.mark.parametrize("app_id", APPS)
def test_every_app_exports_as_the_app_repository_takes_it(app_id):
    theirs = metadata.export(pkglib.load_metadata(os.path.join(ROOT, "apps", app_id), CATEGORIES))
    assert metadata.check(theirs) == []
    assert "whyos" not in theirs


def test_their_rules():
    good = {"name": "Ballz GL", "description": "Ballz demo in RGB888", "categories": ["demo"], "version": "2.3.0",
            "icon": {"16x16": "icon16.png"}, "author": "Someone", "license_type": "CC0-1.0",
            "application": [{"targets": ["tanmatsu"], "type": "script", "revision": 6,
                             "assets": [{"source_file": "app.so", "target_file": "app.so"}]}]}
    assert metadata.check(good) == []
    assert metadata.check(dict(good, description="no: commas, here")) != []
    assert metadata.check(dict(good, name="ab")) != []
    assert metadata.check(dict(good, extra=1)) != []
    assert metadata.check(dict(good, application=[])) != []
    assert metadata.check(dict(good, application=[{"targets": ["x"]}])) != []
    assert metadata.check(dict(good, application=[{"targets": ["x"], "type": "appfs", "revision": "6"}])) != []
    assert metadata.check(dict(good, icon={})) != []


def test_plain_text():
    assert metadata.plain("Café: one, two; three + four?") == "Cafe - one two - three and four"
    assert len(metadata.plain("x" * 400)) == 256


def test_a_launcher_for_the_original_firmware_never_takes_a_whyos_package():
    meta = metadata.from_manifest(pkglib.load_manifest(os.path.join(ROOT, "apps", "calc")), CATEGORIES)
    for a in meta["application"]:
        assert "why2025" not in a["targets"] and a["type"] not in ("appfs", "script", "plugin", "elf")


def test_convert_publishes_the_same_package(repo):
    _, out = publish(repo)
    before = {os.path.relpath(os.path.join(dp, f), out): open(os.path.join(dp, f), "rb").read()
              for dp, _, fs in os.walk(out) for f in fs if "app-repository" not in dp}
    run(os.path.join(ROOT, "tools", "metadata.py"), "convert", "gnuboy", root=repo)
    d = os.path.join(repo, "apps", "gnuboy")
    assert os.path.isfile(os.path.join(d, "metadata.json")) and not os.path.exists(os.path.join(d, "manifest.toml"))
    run(os.path.join(ROOT, "tools", "validate.py"), "gnuboy", root=repo)
    r, _ = publish(repo)
    after = {os.path.relpath(os.path.join(dp, f), out): open(os.path.join(dp, f), "rb").read()
             for dp, _, fs in os.walk(out) for f in fs if "app-repository" not in dp}
    assert before == after and "unchanged (serial 1)" in r.stdout


def test_the_app_repository_folder(repo):
    _, out = publish(repo)
    ar = os.path.join(out, "app-repository")
    theirs = pkglib.load_json(os.path.join(ar, "gnuboy", "metadata.json"))
    assert metadata.check(theirs, os.path.join(ar, "gnuboy")) == []
    projects = pkglib.load_json(os.path.join(ar, "projects.json"))
    assert [p["slug"] for p in projects] == ["gnuboy"]
    pkg = projects[0]["project"]["whyos"]["package"]
    assert pkg["url"].endswith("packages/gnuboy-1.0.0.wpk") and len(pkg["sha256"]) == 64
    assert pkglib.load_json(os.path.join(ar, "categories.json")) == ["emulators", "games"]
    assert pkglib.load_json(os.path.join(ar, "information.json"))["data_path"] == "app-repository"


def test_format_2_problems_are_reported(repo):
    run(os.path.join(ROOT, "tools", "metadata.py"), "convert", "gnuboy", root=repo)
    p = os.path.join(repo, "apps", "gnuboy", "metadata.json")
    meta = pkglib.load_json(p)
    meta["description"] = "Game Boy: with a colon"
    meta["application"][0]["type"] = "appfs"
    meta["whyos"]["version"] = "9"
    metadata.write(p, meta)
    r = run(os.path.join(ROOT, "tools", "validate.py"), "gnuboy", root=repo, check=False)
    assert r.returncode == 1
    assert "characters they do not allow" in r.stdout and "none with targets" in r.stdout
    assert "whyos.version: belongs at the top" in r.stdout
