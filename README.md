# WhyOS-Apps

The app store for Linux on the WHY2025 badge. This repository holds the
catalogue (one directory per app), the category tree, the tooling that
turns them into packages and an index, and the published result in
`dist/v1/`. Publishing is a git push: the store client on the badge reads
the index and downloads packages from this repository over HTTPS
(raw.githubusercontent.com) and installs them on the microSD card.

Status: **specification, draft 1.0** (2026-10-12). `apps/` has the first
five packages (the SD-card apps of why2025-linux), checked with
`tools/validate.py`, and are published in `dist/v1/` by `tools/publish.py`.

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
| [apps/](apps/) | The apps: gnuboy, NetSurf, Links, nmap, imgview |
| [toolchain/](toolchain/README.md) | How the apps' toolchain is built (part of their corresponding source) |
| [tools/](tools/) | `validate.py` (check apps against the spec), `import-sdapp.py` (package an app built by why2025-linux) |

## Submitting an app (once the store is live)

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
