# Roadmap, open questions, non-goals

## Phases

| Phase | What | Done when |
|---|---|---|
| **1. Catalogue and CLI** (done 2026-10-12) | Schemas, `categories.json`, host tools (validate, pack, publish), publishing by git push (`dist/v1/`), a check workflow; the `store` CLI on the badge; the runtime contract in `apprun`/`home`; the existing SD apps (gnuboy, NetSurf, Links, nmap, imgview) repackaged with `artifact.toml` | `store install gnuboy` works on a badge from a clean card, and remove leaves nothing behind |
| **2. Store UI and menu** (done 2026-10-12, see below) | The full-screen UI, a store entry in the menu, grouping by subcategory, more than 24 apps, `store maintain` on card insertion, the store in the OS image | A user can browse, search, install, swap cards and reinstall without a shell |
| **3. Builds from source** | A container image with the badge toolchain for CI; `build.sh` per app; reproducible builds checked by building twice | New versions of the existing apps come from CI only |
| **Later** | Ratings or reactions (GitHub Discussions per app, read-only in the store); delta updates; enforcing `permissions` (needs OS support); app dependencies on shared data packs; localisation of store texts | as needed |

## Status of phases 1 and 2 (2026-10-12)

Checked on a badge (why2025-linux `usb-host`, against this repository on
raw.githubusercontent.com):

- `store install gnuboy` on a clean card (a fresh `STORE_SD`), `verify`,
  `remove`: nothing left behind. Peak memory of an install 920 KB.
- From the UI: browse, search, install (replacing a hand-installed app
  after a question), Open (the menu starts the app or shows its files, with
  `APP_*` set and `%D` expanded), Esc during a download (nothing changed),
  adopt. The UI takes 1.3 MB; a screen draws in 120-250 ms.
- nmap's command over SSH (`$HOME/.whyos/bin` in dropbear's PATH);
  `ROTATION=locked` writes and removes `/tmp/rotation-lock`.
- The store in the OS image registers itself at the first boot (tried by
  putting the image's `/data/apps/store` on a badge, not by flashing the
  data partition, which would wipe the badge's settings).

Covered by the host tests only (`client/tests/`): a card swap (missing,
reinstall), updates, pending cleanups, rollback, damaged packages,
interrupted installs. Not done: a power cut during an install on a badge
(needs a spare card), and tilting the badge while an app holds the
rotation lock.

## Open questions

- **Flash budget for settings.** `/data` has about 1 MB free with the
  store in it, and holds `/etc`, `/root` and the store itself. Is a cap per
  app (default 16 KB, maximum 256 KB) enough, or does the store also need a
  total cap and a warning?
- **Store binary size and speed.** The store is 905 KB in flash (mbedTLS,
  inflate, the UI and its fonts). Downloads run at about 30 KB/s on the
  badge (mbedTLS on the P4; a 1.8 MB package takes about a minute). A
  shared TLS helper for the store, NetSurf and Links is still an option.
- **The UI's source.** The screens are built with the menu's drawing code
  and fonts from why2025-linux (`WHY2025_LINUX`), which is not public yet.
  Until it is (or that code is copied in here), only the CLI builds from
  this repository alone; `dist/` has the built store.
- **Card insertion.** There is no automount: the card is mounted at boot or
  from Settings > Disks. `store maintain` runs at boot, when the store
  starts, and when Disks mounts the card.
- **Moving data of hand-installed apps.** Replacing a hand-installed app
  keeps the data where the old app put it (gnuboy's ROMs in `/mnt/sd/roms`,
  the store version uses `appdata/gnuboy/roms`). The store could offer to
  move it.
- **User folders and Files.** Should `appdata/<id>/<user folder>` show up
  in Files under a friendly name (e.g. "Game Boy ROMs"), so users find
  where to put files? Needs a small addition to the menu.
- **Apps outside `$HOME/.whyos`.** Existing apps write elsewhere
  (NetSurf: `/root/.netsurf`). Repackaging fixes the known ones; a check
  that lists unexpected writes after a test run would catch new ones.
- **Moderation.** Who besides the repository owner may merge, and is
  there a policy on what is accepted (licences, content)?
- **Signatures.** Left out for now (2026-10-12): publishing is a git push,
  and integrity rests on the repository and TLS (repository.md §6). Adding
  a signed index later is a new optional file (`index.json.minisig`) and a
  `KEYS` field in sources; old clients ignore both.

## Open points of the first packages (2026-10-12)

The five apps in `apps/` were built with why2025-linux `sdapps/build.sh`
at commit 2be6d7c and pass `tools/validate.py`. Not yet done or checked:

- **Installed from the store on a badge:** gnuboy, imgview and nmap
  (2026-10-12). NetSurf and Links were not run from the store yet; their
  start scripts fall back to the old behaviour without `APP_CONFIG`.
- **Memory figures** for gnuboy, Links and imgview are estimates; NetSurf
  (about 5 MB) and nmap (7 MB for one port) come from measurements in the
  documentation.
- **Licences.** The MIT texts for the badge's own code name "the
  why2025-linux and WhyOS-Apps contributors" as copyright holder; the
  actual authors should confirm or correct that. nmap's NPSL 0.95 allows
  redistribution under its own terms but is not open source in the OSI
  sense; to be confirmed by a maintainer before publishing. The SPDX id
  chosen for utf8proc's Unicode data terms (`Unicode-DFS-2015`) is
  derived, not checked against the SPDX list. Bundled lists were derived
  from the build script; NetSurf's lists libraries that may not all be
  linked in (listing too many is harmless).
- **NetSurf's tarball is not pinned** by SHA-256 in the build script; its
  hash is recorded in `apps/netsurf/source/SOURCES.md`.
- NetSurf's `opens` includes `svg` (as the hand-installed app did), while
  it is built without SVG support.
- No icons or screenshots yet: the store falls back to the menu icon
  (the UI shows both; tried with a test app on the host).

## Non-goals

- Payments, accounts, and anything needing a server beyond static files.
- Sandboxing: the badge has no MMU and runs everything as root. The
  store keeps apps apart by convention and by its own bookkeeping, not
  by enforcement.
- Automatic background updates: the badge is often on battery and
  offline; updates happen when the user opens the store.
- Other platforms: the store is for WhyOS on the badge only (not BadgeVMS,
  not other hardware). Packages carry no platform field.
- Shared libraries: binaries are static (BFLT); shared code means
  rebuilding apps.
