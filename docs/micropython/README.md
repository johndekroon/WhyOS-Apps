# MicroPython on the badge

Status: 2026-10-28. Covers the store package `micropython` 1.29.0, build 1
([apps/micropython](../../apps/micropython/manifest.toml)).

These pages describe MicroPython as it runs on the WHY2025 badge under
WhyOS. They follow the structure of the official documentation
([docs.micropython.org](https://docs.micropython.org/en/latest/index.html)),
but only cover what this build has and how it behaves on the badge. For the
language itself and for the details of a module, the official
documentation of version 1.29 stays the reference; these pages say which
parts of it apply here.

| Page | What it covers |
|---|---|
| [quickref.md](quickref.md) | Quick reference: starting it, the REPL's keys, where files go, installing packages, short examples |
| [general.md](general.md) | General information about this port: the Unix port on a NOMMU Linux, memory, files, clock, network, environment variables |
| [library.md](library.md) | Library reference: the modules that are built in, the frozen Python modules, and what is left out |
| [reference.md](reference.md) | Language reference: faster code (native, viper), `.mpy` files, memory management, differences from CPython |
| [hardware.md](hardware.md) | The badge's hardware from Python: through Linux's device files, since there is no `machine.Pin` and co. |

## In one paragraph

The badge runs MicroPython's **Unix port**, unchanged, as an ordinary Linux
program: the same MicroPython you would build for a PC, cross-compiled for
the badge's ESP32-P4 (RV32, no MMU). It is not the ESP32 port: there is no
`machine.Pin`, `network` or `bluetooth`, no flash file system of its own,
and no `boot.py`/`main.py`. Files, sockets and processes are Linux's.
Python code reaches the badge's hardware the way any Linux program does,
through files in `/dev` and `/sys` ([hardware.md](hardware.md)).

## What is tested where

| What | Simulator (QEMU, 2026-10-12) | Badge |
|---|---|---|
| Install from the store, `store verify` | yes | yes, 2026-10-13 |
| A script from a shell (`micropython file.py`) | yes | yes, 2026-10-25 |
| The REPL from the menu, in appterm's window | yes | not yet |
| A script from Files (Open with) | yes | not yet |
| Modules in `lib/`, native and viper code, the heap and `MemoryError` | yes | not yet |
| json, re, time, os, hashlib, binascii, random, math, struct, collections, io, asyncio, deflate, select, errno, platform, heapq, termios (imported and used) | yes | not yet |
| A TCP server and client, HTTPS to micropython.org | yes | not yet |
| `mip install` from micropython-lib | yes | not yet |

Sources: `why2025-linux/sdapps/README.md` (section MicroPython),
`Tools/usb/sim/test_s12_micropython.py` (11 tests),
`Tools/usb/hil/test_hil_micropython.py` (written, not yet run in full on
the badge).

## How these pages were made

- **Build options** from `build_micropython` in `why2025-linux/sdapps/build.sh`
  and the configuration of the Unix port's `standard` variant in the
  MicroPython 1.29.0 source (`ports/unix/mpconfigport.mk`,
  `variants/mpconfigvariant_common.h`, `variants/standard/manifest.py`).
- **Module and function names** checked on 2026-10-28 against the names in
  the program itself (`micropython` from this package's `files.tar.gz`).
  A name that is there confirms the feature is built in. A few names
  (`array`, `json`, `time`, `re`, `vfs`) cannot be told apart in the
  program because the linker shares them with longer names; for those the
  build options decide, and the simulator test imports them.
- **Not done:** `help('modules')` on the badge itself. Where a page says
  "derived", it follows from the source or the program, not from a run.

## Open points

- The REPL, Files' Open with, `mip` and HTTPS have not run on the badge
  yet (see the table).
- TLS does not check certificates by default, and the badge has no system
  bundle of CA certificates ([library.md](library.md), `ssl`).
- An install shortly after boot was once reported done while its files
  were missing (the card's bus is set up again 10–35 s after boot;
  `why2025-linux/sdapps/README.md`).
