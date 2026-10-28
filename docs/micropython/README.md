# Python on the badge

MicroPython 1.29 runs on the WHY2025 badge as an app from the store. These
pages are for writing your own Python programs for the badge: from the first
`print("hello")` to a program with its own tile in the menu that reads the
sensors, talks to the internet or types on a computer.

| Page | Read it when you want to |
|---|---|
| [getting-started.md](getting-started.md) | Install MicroPython, try the prompt, write and run your first script, get scripts from your computer onto the badge |
| [programming.md](programming.md) | Write real programs: the terminal window, colours, reading keys, saving data, errors, several things at once, memory, packages, your own menu tile |
| [badge.md](badge.md) | Use the badge: sensors, LEDs, the network, the clock, typing on a computer over USB, serial devices |
| [python.md](python.md) | Know what Python you get: the modules, and what differs from Python on your computer |

For the language and the details of a module, the
[MicroPython documentation](https://docs.micropython.org/en/v1.29.0/) is the
reference; these pages cover what is particular to the badge.

## In short

- It is MicroPython's **Unix port**: MicroPython as a Linux program, the
  same as you would build for a PC. Files, sockets and processes are
  Linux's. It is not the ESP32 port, so there is no `machine.Pin`, no
  `network.WLAN` and no `main.py` that runs at start-up.
- Your scripts live on the SD card in `badge/appdata/micropython/`.
- A program runs in a text window of 73 × 25 characters with 16 colours.
  Drawing graphics on the screen from Python is not possible yet.
- There are no threads; `asyncio` does several things at once.
- A program gets 2 MB of memory by default.

These pages were written on 2026-10-28 from the MicroPython build and the
WhyOS source. Not every example has been run on a badge yet; if one does
not work, that is a bug in these pages.
