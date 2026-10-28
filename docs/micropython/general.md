# General information about MicroPython on WhyOS

Status: 2026-10-28.

## Which MicroPython this is

| | |
|---|---|
| Version | MicroPython 1.29.0, the release tarball, pinned by SHA-256 (`apps/micropython/source/SOURCES.md`) |
| Port | The **Unix port**, variant `standard`, unchanged: only `make` options differ |
| Left out by those options | threads (`_thread`), `ffi`, `btree`, and the FAT and littlefs file systems of MicroPython itself |
| Kept | MicroPython's own line editor (readline), TLS through Mbed TLS 3.6.6, the native code emitter for RV32 (`@micropython.native`, `@micropython.viper`) |
| Program | `micropython`, 0.7 MB, a static BFLT program with a 64 KB stack |
| C library | uClibc-ng 1.0.57 |
| `sys.platform` | `'linux'` |

The build is `build_micropython` in `why2025-linux/sdapps/build.sh`. Why
these options:

- **Threads:** the badge's Linux has no MMU, and uClibc's thread library
  there (LinuxThreads) was not worth it. Use `asyncio` for doing several
  things at once.
- **`ffi`:** needs libffi and shared libraries; the badge runs only static
  programs.
- **FAT and littlefs:** Linux mounts the card and the flash; MicroPython
  reads and writes them as ordinary files.

`platform.platform()` reports `...-with-glibc2.2`: uClibc-ng presents
itself to programs as glibc 2.2. The C library is uClibc-ng all the same
(derived from the program's strings).

## The badge is not a microcontroller here

On an ESP32 board MicroPython is the whole firmware. On the badge it is one
Linux program among others:

- No `boot.py` or `main.py` at start-up, no `machine.reset()` of the
  badge, no `network.WLAN`: Wi-Fi is WhyOS's (Settings > Connection), and
  sockets work as soon as the badge has a network.
- Pins, I2C, SPI and the like belong to Linux drivers. Python reaches
  them through `/dev` and `/sys` ([hardware.md](hardware.md)).
- Ctrl-D leaves MicroPython instead of a soft reset.

## Memory

The badge's Linux runs without an MMU (NOMMU, RV32). That changes a few
things that Python on a PC never notices:

- **One pool for everything.** All programs share a pool of about 13.8 MB
  (`/proc/userpool` shows it); with the menu out of the way about 10 MB is
  free (`why2025-linux/docs/APPS.md`). MicroPython takes its program
  (0.7 MB) plus its heap: 2.8 MB at its peak in the simulator with the
  default 2 MB heap.
- **One block per allocation.** Every program and every heap needs one
  contiguous block of the pool. A heap of 6 MB can fail while 8 MB is free
  in pieces. `MICROPY_HEAP` or `-X heapsize=` sets the heap; 2 MB is the
  default the start script passes.
- **The heap does not grow.** MicroPython takes the whole heap at start;
  past it, Python raises `MemoryError` (tested in the simulator: a 3 MB
  `bytearray` in a 2 MB heap).
- **A wild pointer can hang the badge.** User programs run without memory
  protection. Pure Python is safe, but `viper` code with raw pointers,
  `uctypes` and `machine.mem8/16/32` write wherever they are told to; a
  wrong address there can stop the whole badge instead of raising an
  error (`why2025-linux/docs/RUNTIME-WEDGE.md` explains why user programs
  run without protection).
- **The stack:** 64 KB for the program; MicroPython itself stops recursion
  at 40,000 bytes (`RuntimeError: maximum recursion depth exceeded`).

## Files

Python's `open()`, `os.listdir()` and the like work on Linux's files:

| Path | What | Survives |
|---|---|---|
| `/mnt/sd` | The microSD card: its f2fs partition when the card has the badge layout, else its FAT32/exFAT (`why2025-linux/docs/DATA.md`) | A reboot; travels with the card |
| `/mnt/sd/badge/appdata/micropython` | `APP_DATA`: your scripts, and `lib/` | idem |
| `/media/sd` | The FAT partition of a card in the badge layout, while Files has it open | idem |
| `/root`, `/etc`, `/data` | Flash: about 4.4 MB in all, shared with the store, Write, Files and WhyOS's settings | A reboot and a card swap |
| `/tmp` | RAM, taken from the same pool as programs | Nothing; gone at reboot |

- On a card in the badge layout, `/mnt/sd` is f2fs: a computer that gets
  the card over USB (Settings > Storage & devices) sees only the FAT
  partition, so it does not see your scripts in `badge/appdata`.
- A `.py` file needs no execute bit; `python file.py` runs it. A file with
  `#!/usr/bin/env python` as its first line should run directly where
  `python` is on the `PATH` (a login shell), after `chmod +x`; not tested.
- Keep large writes off `/data` and `/root`: the flash is small and wears.
- `vfs.mount()` works with `vfs.VfsPosix` only; `os.mount` with a FAT or
  littlefs image is not available.

## Clock and time zone

- WhyOS sets the clock by NTP once Wi-Fi is up and keeps trying until NTP
  answers (`why2025-linux/docs/NETWORK.md`). Before that the clock can be
  wrong; `time.time()` then gives that wrong time. Check
  `time.localtime()[0] >= 2026` before you rely on it.
- `time.localtime()` follows the badge's time zone (Settings > General >
  Time zone): WhyOS writes `/etc/TZ`, which uClibc reads
  (`why2025-linux/docs/SETTINGS.md`). Derived; not tested from MicroPython.
- `time.ticks_ms()` and `time.ticks_us()` count from start-up and do not
  jump when NTP sets the clock; use them for measuring time.

## Network

- Wi-Fi is set up in Settings; MicroPython has no `network` module.
  `socket.getaddrinfo()` uses the badge's DNS.
- TCP and UDP clients and servers work through `socket`, TLS through `ssl`
  (simulator, 2026-10-12). IPv6 is not in the kernel
  (`why2025-linux/docs/DATA.md`, kernel partition).
- Over SSH you get the REPL in your own terminal (`ssh root@<badge> python`;
  with `-t` for the line editor).

## Terminal

- From the menu, MicroPython runs in appterm's window: 73 columns by 25
  rows, with the badge's status bar above it, `TERM=vt100`, and a
  VT100/ANSI subset with the 16 ANSI colours (`why2025-linux/docs/APPS.md`).
  The window closes when MicroPython ends with status 0 (Ctrl-D,
  `sys.exit()`); after an error it stays open with the output until Enter
  or Esc (`TERMINAL=close`). The logo key ends both.
- Keys in appterm: the US layout, Ctrl, Alt, arrows, Tab; Fn+↑/↓ scroll
  back 300 lines. The six symbol keys arrive as F1..F6 escape sequences,
  and Esc as an escape character. What MicroPython printed is also in
  `/tmp/app-micropython.log` (derived from the same document).
- `termios` is there (`tcgetattr`, `tcsetattr`, `setraw`), so a script can
  read single keys from `sys.stdin` in raw mode.

## Environment variables

| Variable | Read by | Default | What |
|---|---|---|---|
| `MICROPY_HEAP` | the start script | `2M` | The heap, as `-X heapsize=`: `512K`, `4M` |
| `APP_DATA` | the start script | `/mnt/sd/badge/appdata/micropython` | The scripts folder; `lib/` in it goes on the path. The menu sets it |
| `MICROPYPATH` | MicroPython | `.frozen::$APP_DATA/lib` (set by the start script) | `sys.path`, separated by `:`; an empty entry is the current directory |
| `MICROPYINSPECT` | MicroPython | unset | When set: the REPL after a script, as `-i` |
| `HOME` | MicroPython | `/root` | Where `.micropython.history` goes |

The program reads `os.getenv()` like any other variable; `os.putenv()` and
`os.unsetenv()` change them for programs started with `os.system()`.

## Starting other programs

`os.system("command")` runs a shell command and returns its status. On the
badge it starts the shell with `vfork()`, the only way on NOMMU, and waits
for it. Derived; not tested. There is no `subprocess` module.
