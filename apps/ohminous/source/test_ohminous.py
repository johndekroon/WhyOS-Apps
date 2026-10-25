#!/usr/bin/env python3
"""OHMinous on the host: the screens and keys with its hardware simulated
(ohminous.c, hw.c and bp.c with -DOHM_HOST).

    python3 sdapps/ohminous/test_ohminous.py [screenshots dir]

The badge's pins read what OHM_LEVELS says; I2C devices answer at the
addresses in OHM_I2C; the UART receives OHM_UART_RX, decoded from edges
as on the badge; OHM_BP=1 puts a simulated Bus Pirate on the bottom port.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
O = os.path.join(HERE, "..", "..", "patches", "buildroot", "overlay-src")
if not os.path.isdir(O):        # a copy with the headers next to it (WhyOS-Apps)
    O = HERE

bad = checks = 0


def check(what, got, want):
    global bad, checks
    checks += 1
    if got != want:
        bad += 1
        print("FAIL %s: want %r, got %r" % (what, want, got))


class App:
    def __init__(self, exe, shots):
        self.exe, self.shots = exe, shots
        self.dir = tempfile.mkdtemp()

    def run(self, keys, shot=None, **env):
        e = dict(os.environ, APP_CONFIG=self.dir, OHM_I2C="3c 48 57", OHM_LEVELS="1 0 1 0 0 0",
                 OHM_UART_RX="boot: ready\nI (312) app: sensor ready\n", ASAN_OPTIONS="detect_leaks=0")
        e.pop("OHM_BP", None)
        e.update(env)
        ppm = os.path.join(self.shots or self.dir, (shot or "last") + ".ppm")
        r = subprocess.run([self.exe, keys, ppm], capture_output=True, text=True, env=e)
        if r.returncode or "runtime error" in r.stderr or "Sanitizer" in r.stderr:
            raise SystemExit("ohminous-host failed on %r:\n%s" % (keys, r.stderr))
        st = {}
        lines = r.stdout.splitlines()
        st["screen"] = lines[0]
        for line in lines[1:]:
            k, _, v = line.partition(" ")
            st[k] = v
        return st

    def file(self, name):
        p = os.path.join(self.dir, name)
        return open(p, "rb").read() if os.path.exists(p) else None


def main():
    shots = sys.argv[1] if len(sys.argv) > 1 else ""
    if shots:
        os.makedirs(shots, exist_ok=True)
    exe = os.path.join(tempfile.mkdtemp(), "ohminous-host")
    subprocess.run(["cc", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-format-truncation",
                    "-DOHM_HOST", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                    "-I" + O, "-I" + HERE, "-o", exe] + [os.path.join(HERE, f) for f in ("ohminous.c", "hw.c", "bp.c")],
                   check=True)
    a = App(exe, shots)

    # the start screen, with and without a Bus Pirate
    s = a.run("", shot="start")
    check("start", (s["screen"], s["bp"]), ("start ", "0 0 "))
    s = a.run("", shot="start-bp", OHM_BP="1")
    check("a Bus Pirate there", s["bp"], "1 0 ")
    s = a.run("\\r\\r\\r\\d\\l")
    check("tiles: right stops, down", s["tile"], "4")
    s = a.run("\\e")
    check("Esc closes", "closed" in s, True)
    s = a.run("\\n\\e")
    check("Esc from a tool: the start", ("closed" in s, s["screen"]), (False, "start "))

    # Pins: the levels read, Enter cycles the mode, the triangle toggles an output
    s = a.run("\\n", shot="pins")
    check("pins read", (s["screen"], s["src"], s["pins"]), ("pins ", "badge", "0:1 0:0 0:1 0:0 0:0 0:0"))
    s = a.run("\\n\\d\\n")
    check("IO1 pull-up", s["pins"].split()[1], "1:1")
    s = a.run("\\n\\d\\n\\n\\n\\2")
    check("IO1 an output, high", s["pins"].split()[1], "3:1")
    s = a.run("\\n\\d\\n\\n\\n\\2\\2")
    check("IO1 toggled back low", s["pins"].split()[1], "3:0")
    s = a.run("\\n\\2")
    check("an input does not toggle", s["notice"], "An input: Enter makes it an output")
    s = a.run("\\n\\d\\d\\1\\nsensor\\n", shot="pins-named")
    check("a name", a.file("settings").decode().count("name2=sensor"), 1)
    s = a.run("\\n\\d\\d\\1\\d\\n")
    check("the name back", a.file("settings").decode().count("name2="), 0)

    # Logic: a capture, its options, saved
    s = a.run("\\r\\n\\2", shot="logic")
    check("captured", s["la"], "2048 10000 Captured \u00b7 software")
    s = a.run("\\r\\n\\1\\n\\d\\d\\n\\2")
    check("rate 50 kHz", s["la"].split()[1], "50000")
    s = a.run("\\r\\n\\1\\d\\d\\d\\n\\d\\n\\2")
    check("8k samples", s["la"].split()[0], "8192")
    s = a.run("\\r\\n\\2\\3")
    check("saved", (len(a.file("capture.bin") or b""), s["notice"].startswith("Saved")), (2048, True))
    s = a.run("\\r\\n\\3")
    check("nothing to save", s["notice"], "Nothing captured yet")

    # Buses: I2C scan and registers; the UART
    s = a.run("\\r\\r\\n", shot="buses")
    check("buses", s["screen"], "buses ")
    s = a.run("\\r\\r\\n\\n\\2", shot="i2c")
    check("i2c scan", s["i2c"], "3 3c 48 57")
    s = a.run("\\r\\r\\n\\n\\2", OHM_I2C="")
    check("i2c: nothing", s["i2c"], "0")
    s = a.run("\\r\\r\\n\\n\\2\\d\\n", shot="regs")
    check("registers of 0x48", s["screen"], "regs ")
    s = a.run("\\r\\r\\n\\n\\2\\n\\1" "10 ff\\n")
    check("a register written", s["log"] != "0" and "notice" in s, True)
    s = a.run("\\r\\r\\n\\d\\nstatus\\n", shot="uart")
    check("uart: received and sent", s["term"], "3|boot: ready|I (312) app: sensor ready|\u203a status")
    check("uart: the bytes out", s["uart_tx"], "status")   # with CR LF, which ends the printed line
    s = a.run("\\r\\r\\n\\d\\n\\1\\u\\n")
    check("uart: 4800 baud", a.file("settings").decode().count("uart=2"), 1)
    s = a.run("\\r\\r\\n\\d\\d\\n")
    check("SPI needs a Bus Pirate", s["notice"], "This bus needs a Bus Pirate on the bottom port")

    # LEDs on J7: on, colour, brightness
    s = a.run("\\d\\n\\2", shot="leds")
    check("leds on", s["leds"], "00ff00 00ff00")
    s = a.run("\\d\\n\\2\\r")
    check("leds: next colour", s["leds"], "0000ff 0000ff")

    # Power: the badge's rails; the Bus Pirate's supply
    s = a.run("\\d\\r\\n", shot="power")
    check("power: badge", (s["screen"], s["src"]), ("power ", "badge"))
    s = a.run("\\d\\r\\n\\t", shot="power-missing")
    check("power: no Bus Pirate", (s["src"], s["bp"]), ("bp", "0 0 "))
    s = a.run("\\d\\r\\n\\t\\2", shot="power-bp", OHM_BP="1")
    check("supply on", s["psu"].split()[:3], ["1", "33", "300"])
    s = a.run("\\d\\r\\n\\t\\d\\r\\r", OHM_BP="1")
    check("voltage 3.5 V", s["psu"].split()[1], "35")
    s = a.run("\\d\\r\\n\\t\\d\\d\\l\\l", OHM_BP="1")
    check("limit 280 mA", s["psu"].split()[2], "280")

    # the Bus Pirate's pins and logic
    s = a.run("\\n\\t", shot="pins-bp", OHM_BP="1")
    check("bp pins", (s["src"], s["bp"]), ("bp", "1 1 DIO"))
    s = a.run("\\r\\n\\t\\2", shot="logic-bp", OHM_BP="1")
    check("bp capture", s["la"], "8192 1000000 Captured")

    # Serial: no adapter; with one
    s = a.run("\\d\\r\\r\\n", shot="serial")
    check("serial: no adapter", s["notice"], "No USB serial adapter on the bottom port")
    s = a.run("\\d\\r\\r\\n", OHM_USB="1")
    check("serial: an adapter", s["notice"], "")

    # help, log, settings
    s = a.run("\\6", shot="help")
    check("help", s["screen"], "start help")
    s = a.run("\\r\\n\\2\\e\\4", shot="log")
    check("log", (s["screen"], s["log"]), ("log ", "1"))
    s = a.run("\\5\\n\\d\\n", shot="settings")
    check("settings saved", sorted(a.file("settings").decode().split()[:2]), ["safe=0", "view=1"])
    s = a.run("\\5\\n\\d\\n")
    s = a.run("\\n", shot="pins-back")
    check("map from the back", s["screen"], "pins ")
    s = a.run("", OHM_NO_GPIO="1")
    check("no gpio", s["notice"].startswith("No GPIO here"), True)

    print("%d of %d failed" % (bad, checks))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
