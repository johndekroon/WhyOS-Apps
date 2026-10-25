# WhyOS-Apps

The app store for Linux on the WHY2025 badge. This repository holds the
catalogue (one directory per app), the category tree, the tooling that
turns them into packages and an index, and the published result in
`dist/v1/`. Publishing is a git push: the store client on the badge reads
the index and downloads packages from this repository over HTTPS
(raw.githubusercontent.com) and installs them on the microSD card.

Status (2026-10-12): **phases 1 and 2 done** ([docs/roadmap.md](docs/roadmap.md)).
The specification is draft 1.0. `apps/` has the SD-card apps of
why2025-linux (NetHack, nano, a calculator and MicroPython among them, 2026-10-12) and the store itself,
published in `dist/v1/`. The `store`
client ([client/](client/README.md)) installs, updates and removes them on
a badge, from the command line and from its full-screen UI, which opens
from the menu (Apps > App store, Settings > Storage & devices > My apps). why2025-linux puts
the first store in the image, Write, the text editor ([write/](write/README.md), 2026-10-25), and Files, the file browser ([files/](files/README.md), moved out of the menu 2026-10-25).

## Documents

| Document | What it covers |
|---|---|
| [docs/README.md](docs/README.md) | Overview, goals, terms, how the parts fit together |
| [docs/package.md](docs/package.md) | Package format and manifest |
| [docs/repository.md](docs/repository.md) | This repository: layout, submissions, checks, publishing, index |
| [docs/device.md](docs/device.md) | On the badge: directories, registry, install, update, remove, what an app may rely on |
| [docs/client.md](docs/client.md) | The store client and its UI, and what the OS must provide |
| [docs/roadmap.md](docs/roadmap.md) | Phases, open questions, non-goals |
| [schema/](schema/) | JSON Schemas for the manifest, the index and the category tree |
| [categories.json](categories.json) | The category tree |
| [apps/](apps/) | The apps: gnuboy, NetSurf, Links, nmap, imgview, DOOM, the sensor panel, the Bus Pirate, NetHack, nano, the calculator, HIDeous, BARbarious, MicroPython, Flappy, Marble Orbit, Write, Files, and the store itself |
| [client/](client/README.md) | The `store` program: CLI and UI, for the badge and the host |
| [write/](write/README.md) | Write, the text editor: notes in rich text saved as markdown, code with highlighting; a system app in flash like the store (2026-10-25) |
| [files/](files/README.md) | Files, the file browser the menu's Files tile starts; a system app in flash like the store (2026-10-25, until then part of the menu; not pushed yet) |
| [toolchain/](toolchain/README.md) | How the apps' toolchain is built (part of their corresponding source) |
| [tools/](tools/) | `validate.py` (check apps against the spec), `publish.py` (pack and write `dist/v1/`), `import-sdapp.py` (package an app built by why2025-linux), `serve.py`/`local-source.sh` (a test source) |

## Submitting an app

1. Fork this repository and add `apps/<id>/` with a `manifest.toml`, an
   icon and screenshots ([docs/repository.md](docs/repository.md)).
2. Open a pull request. The check workflow validates the manifest and the
   package.
3. After review, a maintainer runs `tools/publish.py`, commits `apps/` and
   `dist/` together and pushes: that publishes the package and the new index.

## Publishing (maintainers)

```
python3 -m pip install jsonschema pillow pytest    # once
tools/publish.py                  # check, pack and write dist/v1/
python3 -m pytest tests/          # the tools' tests
git add apps dist && git commit && git push
```

`tools/local-source.sh <dir> <host>` serves a test copy over HTTPS (with
a test CA) for trying the store client against it.
