# Quick reference for the badge

Status: 2026-10-28. See [README.md](README.md) for what is tested where.

## Installing

MicroPython is an app in the store: Apps > App store, category Tools
(the manifest says `development`, an alias of Tools in
[categories.json](../../categories.json)), or in a shell:

```
store install micropython
```

It goes to the microSD card (`/mnt/sd/badge/apps/micropython`, 0.7 MB).
The store also puts two commands in `/root/.whyos/bin`, which is on the
`PATH` of every login shell and of commands over SSH: `micropython` and
`python`. Both start the same program through the app's start script.

## Starting it

| From | What happens |
|---|---|
| The menu: Apps > MicroPython | The REPL in appterm's window (73 × 25 characters), in your scripts folder `badge/appdata/micropython`. Ctrl-D closes it, and so does the logo key |
| Files: a `.py` file, Open with > MicroPython | Runs the script, then leaves the REPL open so its output stays readable. Ctrl-D closes the window |
| A shell (serial console or SSH) | `python` starts the REPL in your terminal; `python script.py arg1 arg2` runs a script |

The command line takes the usual options:

```
python script.py          run a file (sys.argv has the arguments)
python -c 'print(1+1)'    run a command
python -m mip install x   run a module (here: install package x)
python -i script.py       run, then stay in the REPL
python -X heapsize=4M     another heap size for this run (default 2M)
python -X emit=native     compile everything as native code
python -O2 script.py      bytecode optimisation level 2
python --version
```

## The REPL

| Key | Does |
|---|---|
| Enter | Runs the line, or continues a block (`...`) |
| Tab | Completes a name; twice lists the choices |
| ↑ ↓ | History (50 lines, kept in `$HOME/.micropython.history`; on the badge `/root`, in flash) |
| Backspace | The REPL indents a new line in a block by itself; Backspace removes that indent one space at a time. Enter on a line with only the indent ends the block |
| Ctrl-C | Interrupts a running program (`KeyboardInterrupt`) |
| Ctrl-D | On an empty line: leaves MicroPython |
| Ctrl-E | Paste mode: paste a block as is, Ctrl-D runs it, Ctrl-C cancels |
| Ctrl-A, Ctrl-B | Raw REPL and back (for tools such as `mpremote`; over the serial console, not tested) |

`help()` prints these; `help(obj)` lists what an object has;
`help('modules')` lists the built-in and frozen modules.

## Where files go

| Path | What |
|---|---|
| `/mnt/sd/badge/appdata/micropython/` | Your scripts. The REPL from the menu starts here, so `import myscript` runs `myscript.py` |
| `/mnt/sd/badge/appdata/micropython/lib/` | Modules you can import from anywhere; `mip` installs here |
| `/mnt/sd/badge/apps/micropython/` | The program and its start script (the store's, do not edit) |

`sys.path` is `['.frozen', '', '/mnt/sd/badge/appdata/micropython/lib']`:
the modules built into the program, then the current directory, then
`lib/`. `MICROPYPATH` overrides it ([general.md](general.md)).

## Installing packages

`mip` installs packages from [micropython-lib](https://github.com/micropython/micropython-lib)
or from GitHub, GitLab and Codeberg into `lib/`. It needs Wi-Fi and the
right time (the clock is set over NTP after Wi-Fi connects).

```
python -m mip install base64
python -m mip install github:org/repo/path/file.py
```

or in the REPL:

```python
import mip
mip.install("base64")
```

Tested in the simulator (`base64`), not yet on the badge. Packages from
micropython-lib's `unix-ffi` section need `ffi`, which this build does not
have. Note: `mip` downloads over HTTPS without checking the server's
certificate ([library.md](library.md), `ssl`).

## Memory

The heap is 2 MB. All programs on the badge share one pool of about
13.8 MB, so ask for more only when needed:

```
MICROPY_HEAP=4M python big.py
```

```python
import gc
gc.collect()
print(gc.mem_free(), gc.mem_alloc())
```

Running out raises `MemoryError`; it does not crash the badge (simulator
test). See [general.md](general.md) for why memory works differently on
this badge.

## Short examples

Files on the card:

```python
with open("/mnt/sd/badge/appdata/micropython/notes.txt", "w") as f:
    f.write("hello badge\n")

import os
print(os.listdir("/mnt/sd/badge/appdata/micropython"))
print(os.statvfs("/mnt/sd"))
```

An HTTP request (`requests` is built in; HTTPS works, without certificate
checks):

```python
import requests
r = requests.get("http://example.com/")
print(r.status_code, r.text[:80])
r.close()
```

A TCP client with plain sockets:

```python
import socket
addr = socket.getaddrinfo("example.com", 80)[0][-1]
s = socket.socket()
s.connect(addr)
s.send(b"HEAD / HTTP/1.0\r\nHost: example.com\r\n\r\n")
print(s.recv(200))
s.close()
```

Several things at once (there are no threads; use `asyncio`):

```python
import asyncio

async def blink(name, ms):
    for i in range(3):
        print(name, i)
        await asyncio.sleep_ms(ms)

async def main():
    await asyncio.gather(blink("a", 100), blink("b", 150))

asyncio.run(main())
```

Time:

```python
import time
t = time.ticks_ms()
time.sleep_ms(50)
print(time.ticks_diff(time.ticks_ms(), t), "ms")
print(time.localtime())
```

JSON and hashes:

```python
import json, hashlib, binascii
print(json.dumps({"badge": "WHY2025", "leds": [1, 2]}))
print(binascii.hexlify(hashlib.sha256(b"abc").digest()))
```
