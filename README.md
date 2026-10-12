# WhyOS-Apps

The app store for Linux on the WHY2025 badge. This repository holds the
catalogue (one directory per app), the category tree, the signing keys and
the tooling that turns them into a published, signed index. The store
client on the badge reads that index, downloads packages from this
repository's releases and installs them on the microSD card.

Status: **specification, draft 1.0** (2026-10-12). `apps/` has the first
five packages (the SD-card apps of why2025-linux), checked with
`tools/validate.py`. Not yet built: CI, publishing, signing, the store
client on the badge, and the OS changes the specification asks for.

## Documents

| Document | What it covers |
|---|---|
| [docs/README.md](docs/README.md) | Overview, goals, terms, how the parts fit together |
| [docs/package.md](docs/package.md) | Package format and manifest |
| [docs/repository.md](docs/repository.md) | This repository: layout, submissions, CI, publishing, signing, index |
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
2. Open a pull request. CI validates the manifest and the package.
3. After review and merge, CI publishes the package and the new index.
