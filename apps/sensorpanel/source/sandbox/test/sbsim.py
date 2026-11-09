"""Running a program under sbsim (sbsim.c): the badge's file system, the run, its report."""
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
WHYOS = os.path.join(HERE, "..", "..")
SIM_C = os.path.join(HERE, "sbsim.c")
INCLUDE = ["-include", os.path.join(HERE, "sbsim.h"), "-I" + HERE, "-I" + os.path.join(HERE, ".."),
           "-I" + os.path.join(WHYOS, "libwb")]


def build(exe, sources, flags=(), include=()):
    """The badge's code of a program, with the sandbox's calls in place of the system's."""
    subprocess.run(["cc", "-O1", "-g", "-w", "-fsanitize=address"] + list(flags) + ["-I" + i for i in include] +
                   INCLUDE + list(sources) + [SIM_C, "-o", exe], check=True)


def tree(root, app, brightness=42):
    """The directories an app finds, as apprun leaves them."""
    for d in ("run", "tmp/app-" + app, "mnt/sd/badge/apps/" + app, "mnt/sd/badge/appdata/%s/.cache" % app,
              "root/.whyos/config/" + app, "sys/class/backlight/backlight"):
        os.makedirs(os.path.join(root, d), exist_ok=True)
    with open(os.path.join(root, "sys/class/backlight/backlight/brightness"), "w") as f:
        f.write("%d\n" % brightness)
    with open(os.path.join(root, "sys/class/backlight/backlight/max_brightness"), "w") as f:
        f.write("255\n")


def run(exe, root, out, app, args=(), end=None, keys=(), shots=(), timeout=120, **env):
    """Runs it to the virtual ms `end`, or to its end; returns the report as a dict."""
    os.makedirs(out, exist_ok=True)
    if os.path.exists(os.path.join(out, "report")):
        os.unlink(os.path.join(out, "report"))
    e = dict(os.environ, SBSIM_ROOT=root, SBSIM_OUT=out, APP_ID=app,
             APP_DIR="/mnt/sd/badge/apps/" + app, APP_DATA="/mnt/sd/badge/appdata/" + app,
             APP_CONFIG="/root/.whyos/config/" + app, APP_CACHE="/mnt/sd/badge/appdata/%s/.cache" % app)
    e.update({k: str(v) for k, v in env.items()})
    if end is not None:
        e["SBSIM_END"] = str(end)
    e["SBSIM_KEYS"] = " ".join("%d:%d" % k for k in keys)
    e["SBSIM_SHOTS"] = " ".join(str(s) for s in sorted(shots))
    p = subprocess.run([exe] + list(args), env=e, capture_output=True, text=True, timeout=timeout)
    if not os.path.exists(os.path.join(out, "report")):
        raise RuntimeError("no report: status %d\n%s" % (p.returncode, p.stderr))
    r = report(os.path.join(out, "report"))
    r["status"] = p.returncode
    r["stderr"] = p.stderr
    return r


def report(path):
    r = {"refused": [], "light": []}
    with open(path) as f:
        for line in f:
            k, _, v = line.rstrip("\n").partition(" ")
            if k == "refused":
                r[k].append(tuple(v.split()[:2]))
            elif k == "light":
                r[k].append(tuple(int(x) for x in v.split()))
            elif k == "first_torn":
                r[k] = v
            else:
                r[k] = int(v)
    return r


def pixels(out, at):
    with open(os.path.join(out, "%d.ppm" % at), "rb") as f:
        return f.read()[len(b"P6\n720 720\n255\n"):]


def pixel(data, x, y):
    return tuple(data[(y * 720 + x) * 3:(y * 720 + x) * 3 + 3])
