"""The store client's lifecycle against a local HTTPS source (docs/client.md §6).

    make -C client test        (Linux: builds store-host, then runs these)

Every test gets a fresh root: $HOME, a "card", /data/apps, /tmp and the
sources.d, all under tmp_path, through the STORE_* variables (paths.c).
The source is tools/publish.py output for synthetic apps, served by
tools/serve.py with a test CA; the store trusts it through CAFILE=.
"""
import hashlib
import json
import os
import shutil
import socket
import subprocess
import sys
import time

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
CLIENT = os.path.dirname(HERE)
REPO = os.path.dirname(CLIENT)
STORE = os.path.join(CLIENT, "store-host")
sys.path.insert(0, os.path.join(REPO, "tools"))
import pkglib  # noqa: E402

HELLO = '''format = 1
id = "hello"
name = "Hello"
version = "{version}"
build = {build}
summary = "Test app"
description = "A test app with everything a manifest can have."
author = "tests"
license = "MIT"
source = "https://example.org/hello"
type = "app"
category = "utilities"
tags = ["test", "greeting"]

[launch]
exec = "hello"
args = "--data %D --conf %C"
files = "%D/roms"
types = ["txt"]
terminal = "shell"

[[commands]]
name = "hello-cmd"
exec = "hello"
args = "--from-shell %d/share"

[requires]
os_api = 1
memory_kb = 10

[storage.config]
max_kb = 8

[storage.data]
folders = ["roms", "roms/saves"]
user_folders = ["roms"]

[storage.cache]
enabled = true

[hooks]
post_install = "hooks/post"
migrate = "hooks/migrate"
pre_remove = "hooks/pre"
'''

TINY = '''format = 1
id = "tiny"
name = "Tiny"
version = "1.0.0"
build = 1
summary = "A small app that lives in flash"
description = "Test."
author = "tests"
license = "MIT"
source = "https://example.org/tiny"
type = "app"
category = "utilities"
install_location = "flash"

[launch]
exec = "tiny"

[requires]
os_api = 1
memory_kb = 1
'''


def write_app(repo, app_id, manifest, files):
    d = os.path.join(repo, "apps", app_id)
    shutil.rmtree(d, ignore_errors=True)
    src = os.path.join(d, "build")
    for rel, data in files.items():
        p = os.path.join(src, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as f:
            f.write(data)
        os.chmod(p, 0o755)
    with open(os.path.join(d, "manifest.toml"), "w") as f:
        f.write(manifest)
    tree, problems = pkglib.collect([src], lambda rel: False)
    assert not problems
    pkglib.write_archive(tree, os.path.join(d, "files.tar.gz"))
    with open(os.path.join(d, "artifact.toml"), "w") as f:
        f.write('path = "files.tar.gz"\nsha256 = "%s"\n' % pkglib.sha256_file(os.path.join(d, "files.tar.gz")))
    shutil.rmtree(src)


def hello_files(version):
    return {
        "hello": "#!/bin/sh\necho hello %s \"$@\"\n" % version,
        "share/data.txt": "data %s\n" % version,
        "hooks/post": '#!/bin/sh\necho "$APP_ID $APP_DIR" > "$APP_DATA/post.txt"\n',
        "hooks/migrate": '#!/bin/sh\necho "$1 $2" > "$APP_CONFIG/migrate.txt"\n',
        "hooks/pre": '#!/bin/sh\n[ -n "$TEST_MARK" ] && echo pre > "$TEST_MARK"\nexit 0\n',
        "LICENSES/hello.txt": "MIT\n",
    }


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Env:
    def __init__(self, tmp):
        self.tmp = tmp
        self.repo = os.path.join(tmp, "repo")
        self.srv = os.path.join(tmp, "srv")
        self.port = free_port()
        self.base = "https://127.0.0.1:%d/v1/" % self.port
        self.root = os.path.join(tmp, "root")
        self.home = os.path.join(self.root, "home")
        self.sd = os.path.join(self.root, "sd")
        self.flash = os.path.join(self.root, "data", "apps")
        self.tmpdir = os.path.join(self.root, "tmp")
        self.etc = os.path.join(self.root, "etc")
        for d in (self.home, self.sd, self.flash, self.tmpdir, os.path.join(self.etc, "sources.d")):
            os.makedirs(d, exist_ok=True)
        with open(os.path.join(self.etc, "whyos-release"), "w") as f:
            f.write('NAME=WhyOS\nVERSION=test\nAPI=1\nFEATURES="wifi sd-card display keyboard"\nSD=/nonexistent\n')
        os.makedirs(self.repo)
        shutil.copy(os.path.join(REPO, "categories.json"), self.repo)
        shutil.copytree(os.path.join(REPO, "schema"), os.path.join(self.repo, "schema"))
        self.ca()
        with open(os.path.join(self.etc, "sources.d", "test.conf"), "w") as f:
            f.write("ID=whyos\nURL=%s\nCAFILE=%s\n" % (self.base, os.path.join(tmp, "ca.pem")))
        self.server = None

    def ca(self):
        t = self.tmp
        run = lambda *a: subprocess.run(a, check=True, capture_output=True)  # noqa: E731
        run("openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes",
            "-days", "30", "-subj", "/CN=test CA", "-keyout", t + "/ca.key", "-out", t + "/ca.pem")
        run("openssl", "req", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes",
            "-subj", "/CN=127.0.0.1", "-keyout", t + "/srv.key", "-out", t + "/srv.csr")
        with open(t + "/srv.ext", "w") as f:
            f.write("subjectAltName=IP:127.0.0.1\nbasicConstraints=CA:FALSE\n")
        run("openssl", "x509", "-req", "-in", t + "/srv.csr", "-CA", t + "/ca.pem", "-CAkey", t + "/ca.key",
            "-CAcreateserial", "-days", "30", "-extfile", t + "/srv.ext", "-out", t + "/srv.pem")

    def publish(self):
        env = dict(os.environ, WHYOS_APPS_ROOT=self.repo)
        r = subprocess.run([sys.executable, os.path.join(REPO, "tools", "publish.py"), "--out",
                            os.path.join(self.srv, "v1"), "--base-url", self.base],
                           env=env, capture_output=True, text=True)
        assert r.returncode == 0, r.stdout + r.stderr

    def serve(self, *extra):
        self.stop()
        self.log = open(os.path.join(self.tmp, "server.log"), "a")
        self.server = subprocess.Popen(
            [sys.executable, os.path.join(REPO, "tools", "serve.py"), self.srv, self.tmp + "/srv.pem",
             self.tmp + "/srv.key", "--port", str(self.port), "--bind", "127.0.0.1", *extra],
            stdout=self.log, stderr=self.log)
        for _ in range(50):
            try:
                socket.create_connection(("127.0.0.1", self.port), 0.2).close()
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("test server did not start")

    def stop(self):
        if self.server:
            self.server.terminate()
            self.server.wait()
            self.server = None

    def store(self, *args, check=True, sd=None, extra_env=None):
        env = dict(os.environ, HOME=self.home, STORE_SD=sd or self.sd, STORE_FLASH_APPS=self.flash,
                   STORE_TMP=self.tmpdir, STORE_SYS_SOURCES=os.path.join(self.etc, "sources.d"),
                   STORE_RELEASE=os.path.join(self.etc, "whyos-release"),
                   ASAN_OPTIONS="detect_leaks=0")
        env.update(extra_env or {})
        r = subprocess.run([STORE, *args], env=env, capture_output=True, text=True, timeout=120)
        if check:
            assert r.returncode == 0, "store %s -> %d\n%s%s" % (" ".join(args), r.returncode, r.stdout, r.stderr)
        assert "AddressSanitizer" not in r.stderr and "runtime error" not in r.stderr, r.stderr
        return r

    # paths on the "badge"
    def app(self, app_id):
        return os.path.join(self.sd, "badge", "apps", app_id)

    def data(self, app_id):
        return os.path.join(self.sd, "badge", "appdata", app_id)

    def conf(self, app_id):
        return os.path.join(self.home, ".whyos", "config", app_id)

    def reg(self, app_id):
        return os.path.join(self.home, ".whyos", "store", "installed", app_id + ".json")

    def wrapper(self, name):
        return os.path.join(self.home, ".whyos", "bin", name)


@pytest.fixture
def env(tmp_path):
    if not os.path.exists(STORE):
        pytest.skip("no store-host: make -C client host")
    e = Env(str(tmp_path))
    write_app(e.repo, "hello", HELLO.format(version="1.0.0", build=1), hello_files("1.0.0"))
    write_app(e.repo, "tiny", TINY, {"tiny": "#!/bin/sh\necho tiny\n", "LICENSES/tiny.txt": "MIT\n"})
    e.publish()
    e.serve()
    yield e
    e.stop()


def nothing_left(e, app_id):
    left = [p for p in (e.app(app_id), e.data(app_id), e.conf(app_id), e.reg(app_id),
                        e.wrapper("hello-cmd")) if os.path.exists(p)]
    assert not left, left


def test_update_search_info(env):
    r = env.store("update")
    assert "whyos: 2 apps (serial 1)" in r.stdout
    assert "hello" in env.store("search", "greeting").stdout
    assert "tiny" not in env.store("search", "greeting").stdout
    assert "hello-cmd" in env.store("info", "hello").stdout
    j = json.loads(env.store("--json", "search", "hello").stdout)
    assert j[0]["id"] == "hello" and j[0]["state"] == "not installed"


def test_install_writes_everything(env):
    env.store("install", "hello")
    a = env.app("hello")
    conf = open(os.path.join(a, "app.conf")).read()
    for line in ('NAME="Hello"', 'EXEC="hello"', 'ARGS="--data %D --conf %C"', 'FILES="%D/roms"',
                 'TYPES="txt"', 'TERMINAL="shell"', 'ID="hello"', 'TYPE="app"', 'SUBCATEGORY="utilities"',
                 'CATEGORY="apps"', 'ROTATION="auto"'):
        assert line in conf, line
    assert json.load(open(os.path.join(a, ".package", "manifest.json")))["id"] == "hello"
    assert open(os.path.join(a, ".package", "source")).read().startswith("ID=whyos\nURL=https://")
    assert os.path.isdir(os.path.join(env.data("hello"), "roms", "saves"))
    assert os.path.isdir(os.path.join(env.data("hello"), ".cache"))
    assert os.path.isdir(env.conf("hello"))
    assert open(os.path.join(env.data("hello"), "post.txt")).read().startswith("hello " + a)
    reg = json.load(open(env.reg("hello")))
    card = json.load(open(os.path.join(env.sd, "badge", ".store", "card.json")))["card"]
    assert reg["version"] == "1.0.0" and reg["build"] == 1 and reg["cards"] == [card]
    w = open(env.wrapper("hello-cmd")).read()
    assert "# written by store for hello" in w and w.rstrip().endswith(
        'exec "$APP_DIR/hello" "--from-shell" "%s/share" "$@"' % a)
    out = subprocess.run(["sh", env.wrapper("hello-cmd"), "x"], capture_output=True, text=True).stdout
    assert out.strip() == "hello 1.0.0 --from-shell %s/share x" % a
    assert not os.listdir(os.path.join(env.sd, "badge", ".store", "download"))
    assert "is installed and up to date" in env.store("install", "hello").stdout


def test_verify_and_repair(env):
    env.store("install", "hello")
    with open(os.path.join(env.app("hello"), "share", "data.txt"), "w") as f:
        f.write("changed\n")
    r = env.store("verify", "hello", check=False)
    assert r.returncode != 0 and "share/data.txt changed" in r.stdout
    assert "needs repair" not in env.store("list").stdout   # changed data is not a missing program
    env.store("repair", "hello")
    assert open(os.path.join(env.app("hello"), "share", "data.txt")).read() == "data 1.0.0\n"
    assert env.store("verify", "hello").returncode == 0


def test_update_keeps_data_and_runs_migrate(env):
    env.store("install", "hello")
    open(os.path.join(env.data("hello"), "roms", "game.txt"), "w").write("mine")
    open(os.path.join(env.conf("hello"), "settings"), "w").write("x")
    write_app(env.repo, "hello", HELLO.format(version="1.1.0", build=2), hello_files("1.1.0"))
    env.publish()
    env.store("update")
    assert "1.0.0 -> 1.1.0" in env.store("list", "--updates").stdout
    env.store("install", "hello")
    assert open(os.path.join(env.app("hello"), "share", "data.txt")).read() == "data 1.1.0\n"
    assert open(os.path.join(env.data("hello"), "roms", "game.txt")).read() == "mine"
    assert open(os.path.join(env.conf("hello"), "settings")).read() == "x"
    assert open(os.path.join(env.conf("hello"), "migrate.txt")).read().strip() == "1 2"
    assert json.load(open(env.reg("hello")))["build"] == 2
    assert not os.path.exists(os.path.join(env.sd, "badge", "apps", ".hello.old"))
    # going back needs the version named, and --yes
    r = env.store("install", "hello@1.0.0", check=False)
    assert r.returncode == 2 and "--yes" in r.stderr
    env.store("--yes", "install", "hello@1.0.0")
    assert json.load(open(env.reg("hello")))["build"] == 1


def test_pin(env):
    env.store("install", "hello")
    env.store("pin", "hello")
    write_app(env.repo, "hello", HELLO.format(version="1.1.0", build=2), hello_files("1.1.0"))
    env.publish()
    env.store("update")
    assert "hello" not in env.store("list", "--updates").stdout
    env.store("unpin", "hello")
    assert "hello" in env.store("list", "--updates").stdout


def test_remove_asks_for_user_files_and_leaves_nothing(env):
    env.store("install", "hello")
    open(os.path.join(env.data("hello"), "roms", "game.txt"), "w").write("mine")
    open(os.path.join(env.tmpdir, "app-hello.log"), "w").write("log")
    r = env.store("remove", "hello", check=False)
    assert r.returncode == 2 and "1 file(s) in roms" in r.stderr
    assert os.path.isdir(env.app("hello"))
    mark = os.path.join(env.tmp, "pre-remove")
    env.store("--yes", "remove", "hello", extra_env={"TEST_MARK": mark})
    assert open(mark).read().strip() == "pre"
    nothing_left(env, "hello")
    assert not os.path.exists(os.path.join(env.tmpdir, "app-hello.log"))


def test_card_swap_missing_reinstall(env):
    env.store("install", "hello")
    old = env.sd
    new = os.path.join(env.root, "sd2")
    os.makedirs(new)
    j = json.loads(env.store("--json", "list", sd=new).stdout)
    assert j[0]["state"] == "not on this card"
    assert json.loads(env.store("--json", "list", "--missing", sd=new).stdout)[0]["id"] == "hello"
    env.store("install", "hello", sd=new)
    cards = json.load(open(env.reg("hello")))["cards"]
    assert len(cards) == 2
    assert os.path.isdir(os.path.join(new, "badge", "apps", "hello"))
    # without a card at all: missing, and the config dir stays
    assert json.loads(env.store("--json", "list", sd=os.path.join(env.root, "nocard")).stdout)[0][
        "state"] == "not on this card"
    assert os.path.isdir(env.conf("hello")) and os.path.isdir(old)


def test_remove_without_the_card_cleans_up_later(env):
    env.store("install", "hello")
    other = os.path.join(env.root, "sd2")
    os.makedirs(other)
    env.store("--yes", "remove", "hello", sd=other)
    assert not os.path.exists(env.reg("hello")) and not os.path.exists(env.conf("hello"))
    assert os.path.isdir(env.app("hello"))           # still on the absent card
    assert "hello" in env.store("list", sd=other).stdout   # as a pending cleanup
    r = env.store("maintain")                          # the card is back
    assert "deleted 1 app" in r.stdout
    nothing_left(env, "hello")
    assert not os.path.exists(os.path.join(env.home, ".whyos", "store", "cleanup.json"))


def test_reused_card_is_not_touched(env):
    env.store("install", "hello")
    other = os.path.join(env.root, "sd2")
    os.makedirs(other)
    env.store("--yes", "remove", "hello", sd=other)
    # meanwhile the card got something else called hello
    m = os.path.join(env.app("hello"), ".package", "manifest.json")
    mj = json.load(open(m))
    mj["id"] = "someone-else"
    json.dump(mj, open(m, "w"))
    env.store("maintain")
    assert os.path.isdir(env.app("hello")) and os.path.isdir(env.data("hello"))


def test_interrupted_install_is_undone_or_finished(env):
    env.store("install", "hello")
    apps = os.path.join(env.sd, "badge", "apps")
    # a power cut while unpacking 1.1.0: journal + half a new directory
    with open(os.path.join(env.sd, "badge", ".store", "journal"), "w") as f:
        f.write("install hello 2\n")
    os.makedirs(os.path.join(apps, ".hello.new", "share"))
    r = env.store("maintain")
    assert "undone" in r.stdout
    assert not os.path.exists(os.path.join(apps, ".hello.new"))
    assert env.store("verify", "hello").returncode == 0
    # a power cut between the two renames: the old one aside, no new one
    os.rename(env.app("hello"), os.path.join(apps, ".hello.old"))
    env.store("maintain")
    assert env.store("verify", "hello").returncode == 0


def test_interrupted_install_that_never_landed(env):
    # the card went away after the registry was written but before the
    # files were on it (seen on a badge, 2026-10-12): undone means gone
    env.store("install", "hello")
    with open(os.path.join(env.sd, "badge", ".store", "journal"), "w") as f:
        f.write("install hello 1\n")
    shutil.rmtree(env.app("hello"))
    assert "undone" in env.store("maintain").stdout
    assert not os.path.exists(env.reg("hello"))
    assert not os.path.exists(env.wrapper("hello-cmd"))
    assert "hello" not in env.store("list").stdout


def test_interrupted_update_goes_back_to_the_old_version(env):
    env.store("install", "hello")
    apps = os.path.join(env.sd, "badge", "apps")
    reg = json.load(open(env.reg("hello")))
    reg["version"], reg["build"] = "1.1.0", 2     # registry already says the new one
    with open(env.reg("hello"), "w") as f:
        json.dump(reg, f)
    with open(os.path.join(env.sd, "badge", ".store", "journal"), "w") as f:
        f.write("install hello 2\n")
    os.rename(env.app("hello"), os.path.join(apps, ".hello.old"))
    assert "undone" in env.store("maintain").stdout
    reg = json.load(open(env.reg("hello")))
    assert (reg["version"], reg["build"]) == ("1.0.0", 1)
    assert env.store("verify", "hello").returncode == 0


def test_adopt_and_legacy(env):
    env.store("install", "hello")
    os.unlink(env.reg("hello"))
    j = json.loads(env.store("--json", "list", "--other").stdout)
    assert {"id": "hello", "state": "found on this card"}.items() <= j[0].items()
    env.store("adopt", "hello")
    assert json.load(open(env.reg("hello")))["version"] == "1.0.0"
    leg = os.path.join(env.sd, "badge", "apps", "handmade")
    os.makedirs(leg)
    open(os.path.join(leg, "app.conf"), "w").write('NAME="Handmade"\nEXEC=run\n')
    assert "handmade     other app" in env.store("list").stdout
    assert env.store("remove", "handmade", check=False).returncode == 1
    env.store("delete", "handmade")
    assert not os.path.exists(leg)


def test_flash_location(env):
    env.store("install", "tiny", sd=os.path.join(env.root, "nocard"))   # no card needed
    t = os.path.join(env.flash, "tiny")
    assert os.access(os.path.join(t, "tiny"), os.X_OK)
    assert json.load(open(env.reg("tiny")))["location"] == "flash"
    env.store("remove", "tiny")
    assert not os.path.exists(t) and not os.path.exists(env.reg("tiny"))


def test_older_index_is_refused(env):
    env.store("update")
    idx = os.path.join(env.srv, "v1", "index.json")
    j = json.load(open(idx))
    j["serial"] = 0
    data = json.dumps(j).encode()
    open(idx, "wb").write(data)
    import gzip
    open(idx + ".gz", "wb").write(gzip.compress(data))
    r = env.store("update", check=False)
    assert r.returncode == 1 and "older than the one we have" in r.stderr


def test_tampered_package_is_refused(env):
    pkg = os.path.join(env.srv, "v1", "packages", "hello-1.0.0.wpk")
    data = bytearray(open(pkg, "rb").read())
    data[-20] ^= 1
    open(pkg, "wb").write(data)
    r = env.store("install", "hello", check=False)
    assert r.returncode == 1 and "does not match the index" in r.stderr
    nothing_left(env, "hello")


def test_resume_after_a_dropped_connection(env):
    env.store("update")                 # the index whole first
    env.serve("--drop-after", "600")
    pkg = os.path.join(env.srv, "v1", "packages", "hello-1.0.0.wpk")
    assert os.path.getsize(pkg) > 600
    r = env.store("install", "hello", check=False)
    assert r.returncode == 1 and "dropped" in r.stderr
    part = os.path.join(env.sd, "badge", ".store", "download", "hello-1.0.0.wpk.part")
    assert os.path.getsize(part) == 600
    env.serve()
    env.store("install", "hello")
    assert "range /v1/packages/hello-1.0.0.wpk bytes=600-" in open(os.path.join(env.tmp, "server.log")).read()
    assert env.store("verify", "hello").returncode == 0


def test_redirected_download(env):
    env.serve("--redirect", "/v1/packages/=https://127.0.0.1:%d/v1/moved/" % env.port)
    os.rename(os.path.join(env.srv, "v1", "packages"), os.path.join(env.srv, "v1", "moved"))
    env.store("install", "hello")
    assert env.store("verify", "hello").returncode == 0


def test_untrusted_server_is_refused(env):
    conf = os.path.join(env.etc, "sources.d", "test.conf")
    open(conf, "w").write("ID=whyos\nURL=%s\n" % env.base)     # no CAFILE
    r = env.store("update", check=False)
    assert r.returncode == 1 and "not trusted" in r.stderr


def test_bad_packages_from_the_tools(env, tmp_path):
    out = tmp_path / "bad"
    subprocess.run([sys.executable, os.path.join(REPO, "tests", "make_bad_packages.py"), str(out)], check=True)
    for name, verdict in json.load(open(out / "cases.json")).items():
        r = env.store("check-package", str(out / (name + ".wpk")), check=False)
        assert (r.returncode == 0) == (verdict == "accept"), (name, r.stdout)


def test_replacing_a_hand_installed_app_asks(env):
    leg = env.app("hello")
    os.makedirs(leg)
    open(os.path.join(leg, "app.conf"), "w").write('NAME="Hello by hand"\nEXEC=run\n')
    r = env.store("install", "hello", check=False)
    assert r.returncode == 2 and "installed by hand" in r.stderr
    assert open(os.path.join(leg, "app.conf")).read().startswith('NAME="Hello by hand"')
    env.store("--yes", "install", "hello")
    assert env.store("verify", "hello").returncode == 0
    assert not os.path.exists(os.path.join(env.sd, "badge", "apps", ".hello.old"))



STORE_APP = '''format = 1
id = "store"
name = "App store"
version = "0.2.0"
build = 1
summary = "The store"
description = "Test."
author = "tests"
license = "MIT"
source = "https://example.org/store"
type = "system"
category = "store"
install_location = "flash"

[launch]
exec = "store"

[requires]
os_api = 1
memory_kb = 1
'''


def test_maintain_adopts_the_store_an_image_brought(env):
    write_app(env.repo, "store", STORE_APP, {"store": "#!/bin/sh\necho store\n", "LICENSES/store.txt": "MIT\n"})
    env.publish()
    env.store("--yes", "install", "store")
    os.unlink(env.reg("store"))          # as an OS image leaves it: files and .package/, no registry
    assert "found on this card" in env.store("list").stdout
    r = env.store("maintain")
    assert "registered itself" in r.stdout
    assert os.path.isfile(env.reg("store"))
    assert "installed" in env.store("list").stdout


def test_system_apps_are_not_removed(env):
    # the store, Files and Write come with the image; removed, the menu's
    # tiles break and only a new image brings them back
    write_app(env.repo, "store", STORE_APP, {"store": "#!/bin/sh\necho store\n", "LICENSES/store.txt": "MIT\n"})
    env.publish()
    env.store("--yes", "install", "store")
    s = os.path.join(env.flash, "store")
    r = env.store("--yes", "remove", "store", check=False)
    assert r.returncode == 1 and "comes with WhyOS" in r.stderr, r.stderr
    assert os.path.isfile(os.path.join(s, "store")) and os.path.isfile(env.reg("store"))
    os.unlink(env.reg("store"))          # as an image leaves it, before maintain
    r = env.store("delete", "store", check=False)
    assert r.returncode == 1 and "comes with WhyOS" in r.stderr, r.stderr
    assert os.path.isfile(os.path.join(s, "store"))
    env.store("adopt", "store")
    env.store("install", "tiny")                        # other apps in flash still go
    env.store("--yes", "remove", "tiny")
    assert not os.path.exists(os.path.join(env.flash, "tiny"))


WRAPPED = '''format = 1
id = "wrapped"
name = "Wrapped"
version = "1.0.0"
build = 1
summary = "A start script that runs the real program"
description = "Test."
author = "tests"
license = "MIT"
source = "https://example.org/wrapped"
type = "app"
category = "utilities"

[launch]
exec = "start"

[requires]
os_api = 1
memory_kb = 1
'''


def wrapped(env):
    """An app as micropython, nano and links are: launch.exec is a script
    that runs other programs of the app, which the manifest does not name."""
    write_app(env.repo, "wrapped", WRAPPED, {
        "start": '#!/bin/sh\nexec "${0%/*}/prog" "$@"\n',
        "prog": "bFLT" + "\x00\x00\x00\x04" + "x" * 60,
        "lib/elf": "\x7fELF" + "y" * 60,
        "lib/helper": "#!/bin/sh\necho helper\n",
        "data.txt": "text that mentions bFLT\n",
        "LICENSES/wrapped.txt": "MIT\n"})
    env.publish()
    env.store("install", "wrapped")
    return env.app("wrapped")


def test_programs_the_manifest_does_not_name_run(env):
    # packages carry no modes (pkglib); on a card that is not FAT (f2fs)
    # a program without x cannot run: the client finds them by their start
    a = wrapped(env)
    for rel in ("start", "prog", "lib/elf", "lib/helper"):
        assert os.access(os.path.join(a, rel), os.X_OK), rel
    for rel in ("data.txt", "app.conf", "LICENSES/wrapped.txt"):
        assert not os.access(os.path.join(a, rel), os.X_OK), rel


def test_maintain_makes_installed_programs_runnable(env):
    # installs by store 0.2.10 and older on an f2fs card left them 0644
    a = wrapped(env)
    for rel in ("prog", "lib/helper"):
        os.chmod(os.path.join(a, rel), 0o644)
    env.store("maintain")
    for rel in ("prog", "lib/helper"):
        assert os.access(os.path.join(a, rel), os.X_OK), rel
    assert not os.access(os.path.join(a, "data.txt"), os.X_OK)


def test_maintain_adopts_every_app_an_image_put_in_flash(env):
    # why2025-linux stage-apps.py brings the store and Write: files and
    # .package/ in /data/apps, no registry entry
    env.store("install", "tiny")
    env.store("install", "hello")
    os.unlink(env.reg("tiny"))
    os.unlink(env.reg("hello"))
    r = env.store("maintain")
    assert "tiny 1.0.0 adopted" in r.stdout
    assert json.load(open(env.reg("tiny")))["location"] == "flash"
    # an app on a card from another badge stays the user's choice (device.md §5.6)
    assert not os.path.exists(env.reg("hello"))
