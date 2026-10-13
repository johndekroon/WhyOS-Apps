# This repository: catalogue, CI and publishing

## 1. Layout

```
apps/<id>/manifest.toml        the manifest (package.md §2)
apps/<id>/icon.png             optional: 256×256 PNG with transparency (without it: the menu icon of launch.icon)
apps/<id>/screenshots/<n>.png  optional: up to 6, 720×720 PNG (the panel), named 1.png, 2.png, ...
apps/<id>/build.sh             EITHER: builds the package's files/ from pinned sources
apps/<id>/artifact.toml        OR: a prebuilt files/ archive: path in this repository or URL, + SHA-256
apps/<id>/files.tar.gz         the prebuilt files/ archive, when artifact.toml points to it
apps/<id>/overlay/             optional: files added on top of the build output (wrapper scripts)
apps/<id>/licenses/            licence texts that go into files/LICENSES/ (package.md §2.10)
apps/<id>/source/              badge-specific source and SOURCES.md, when not published elsewhere (§1.2)
apps/<id>/README.md            optional notes for maintainers (not shown in the store)
categories.json                the category tree (§3)
featured.json                  optional: app ids for the store's Discover page
dist/v1/                       the published source (§5), written by tools/publish.py
schema/                        JSON Schemas
toolchain/                     how the apps' toolchain is built: Buildroot config, uClibc patch, build script
tools/                         validate.py, import-sdapp.py, pack.py, publish.py, serve.py, local-source.sh
tests/                         tests of the tools, and the good and malicious packages the client is tested with
docs/                          this specification
.github/workflows/             the check workflow (§4)
```

- One directory per app id. The directory name MUST equal `id`.
- A removed app keeps a tombstone: `apps/<id>/manifest.toml` with
  `withdrawn = "<reason>"`. Its id is never reused, and clients offer
  removal to anyone who has it installed.

### 1.1 Prebuilt or built from source

Two ways to supply the program:

- **`artifact.toml`** (phase 1): the author builds the app with the badge
  toolchain and supplies a `files/` archive (tar.gz): committed in the
  app directory as `files.tar.gz` (`path`), or published somewhere stable
  (`url`). `artifact.toml` pins it with its SHA-256. CI checks the hash
  and packs it. Committed archives keep every version in the git history;
  that is accepted for now (a few MB per version) and can move to
  `url` or Git LFS later without a format change.
- **`build.sh`** (phase 3): CI builds from pinned sources in a container
  with the badge toolchain ([roadmap.md](roadmap.md)). The script gets
  `$OUT` (the `files/` directory to fill) and the toolchain
  in `$PATH`; it MUST pin every download by version and SHA-256.

```toml
# apps/gnuboy/artifact.toml
path = "files.tar.gz"               # or: url = "https://…/files.tar.gz"
sha256 = "…"
built_from = "why2025-linux sdapps/build.sh at 2be6d7c"   # free text: what produced it
```

`tools/import-sdapp.py` makes such an archive from an app built with
why2025-linux `sdapps/build.sh`: it drops `app.conf` and debug files,
adds `overlay/` and `licenses/`, writes a reproducible archive (sorted,
fixed timestamps and owners) and updates `artifact.toml`.

`tools/validate.py` runs the checks of §4 that need no network (schema,
category, archive rules, programs, licence files, source) on all or the
given apps; CI will run it on every pull request.

### 1.2 Source in this repository

When the badge-specific source of an app is not published elsewhere at a
fixed revision, `apps/<id>/source/` holds it: the patches and new files,
the build script that was used, and `SOURCES.md` with every upstream
download (URL and SHA-256, or repository and commit) and how to build.
Together with those downloads and [toolchain/](../toolchain/README.md)
(shared by all apps: every one links the badge's patched uClibc-ng) this
is the complete corresponding source ([package.md](package.md) §2.10).

## 2. Submitting and reviewing

- Changes come in through pull requests. A new app, a new version
  (raise `version` and `build`), and metadata fixes are separate kinds
  of change; CI labels them.
- The `maintainer` in a manifest owns that app: a pull request changing
  `apps/<id>/` from someone else needs their approval or a store
  maintainer's.
- Reviewers check, beyond CI: the license allows redistribution, the
  description is accurate, the app stays within its declared storage, and
  it does not harm the badge (the badge has no memory protection; see
  why2025-linux `docs/KNOWN-ISSUES.md`).

## 3. Categories

`categories.json` ([schema](../schema/categories.schema.json)) is the
fixed tree of types and categories. Manifests may only use what is in it,
so the store's tabs stay tidy: at most four categories per type
(2026-10-13), including `other`. Categories are for browsing the store;
the start menu lists installed apps by name only.

```json
{
  "format": 1,
  "types": [
    { "id": "game", "name": "Games", "icon": "games", "menu": "games",
      "categories": [
        { "id": "emulators", "name": "Emulators", "icon": "gameboy" }
      ] }
  ]
}
```

- `id`s are permanent. A category can be renamed (`name`) freely. To
  retire one, list its id under `aliases` of the category that takes it
  over: manifests that name it stay valid, and `publish.py` writes the
  new id into the index and the app's details. A published package keeps
  the id it was built with (a package never changes); new versions
  should use the new id. On 2026-10-13 eleven app and seven game
  categories became four each this way.
- `menu` says where the start menu lists apps of this type or category
  by default (`apps`, `games`, `tools`, `none`); a category may override
  its type's. No category uses `tools` (2026-10-12): Settings > Tools in the
  menu is for the badge's own utilities, and apps from the store, network
  tools such as nmap included, belong under Apps or Games.
- `icon` is one of the menu's built-in icon names.
- Each type has a category `other` as a fallback.

## 4. Checking and publishing

Publishing is a git push; there is no publishing job and no secret.

**Publish** (a maintainer, after review):

1. `tools/publish.py` runs the checks of `validate.py` on every app
   (schema, category, archive rules, programs, licence files, source),
   then for each app:
   - refuses a version that is already published with different contents
     (raise `version` and `build`), or a `build` not higher than the
     published ones;
   - packs the new version into `dist/v1/packages/<id>-<version>.wpk`
     (`manifest.json` first, generated fields added) and keeps older
     versions;
   - writes `dist/v1/apps/<id>.json`, the licence texts, the source
     archive and the QOI images;
   then rewrites `dist/v1/index.json(.gz)`, raising `serial` only when
   something changed. Running it twice changes nothing.
2. Commit `apps/` and `dist/` together, and push to `main`. That is the
   publication.

**Check** (`.github/workflows/check.yml`, every push and pull request):
`validate.py`, the tests in `tests/`, the client's package checker
(built for the host) on every package in `dist/`, and a check that `dist/`
is what `publish.py` makes from `apps/` (so a forgotten publish fails).

Not yet: building packages from source in CI (phase 3, roadmap).

## 5. Published files

Base URL of the default source:
`https://raw.githubusercontent.com/johndekroon/WhyOS-Apps/main/dist/v1/`
(the repository must be public). GitHub caches these files for a few
minutes, so a push takes that long to reach badges.

```
v1/index.json                      the index (§5.1)
v1/index.json.gz                   the same, gzip; clients SHOULD fetch this one
v1/apps/<id>.json                  details per app (§5.2)
v1/packages/<id>-<version>.wpk     the packages, every published version
v1/sources/<id>-<version>-source.tar.gz   the source archive, for apps with source/
v1/licenses/<id>/<file>            licence texts of the latest version
v1/icons/<id>.qoi                  icon, 96×96, for lists and the info page
v1/icons/<id>@2.qoi                icon, 192×192, for a featured banner
v1/screens/<id>-<n>.qoi            screenshot, 360×360 (half size, fits the info page)
v1/categories.json                 copy of the category tree
```

URLs of packages in the index are absolute, so a mirror can host
packages elsewhere, and packages could later move to release assets or
Git LFS without a format change. Every published version stays in git;
that is accepted for now (a few MB per version).

### 5.1 Index

Schema: [schema/index.schema.json](../schema/index.schema.json). The index
is everything the store needs for lists and search, and nothing more, so
it stays small (aim: under 1 KB per app).

```json
{
  "format": 1,
  "source": "whyos",
  "serial": 1042,
  "generated": "2026-10-12T10:00:00Z",
  "base_url": "https://raw.githubusercontent.com/johndekroon/WhyOS-Apps/main/dist/v1/",
  "categories_sha256": "…",
  "apps": [
    {
      "id": "gnuboy",
      "name": "Game Boy",
      "summary": "Game Boy and Game Boy Color emulator",
      "type": "game",
      "category": "emulators",
      "tags": ["emulator", "gameboy"],
      "terminal": false,
      "commands": [],
      "author": "gnuboy authors",
      "updated": "2026-10-12",
      "icon": "icons/gnuboy.qoi",
      "details_sha256": "…",
      "requires": { "os_api": 1, "memory_kb": 1200, "features": ["sd-card"] },
      "latest": {
        "version": "1.0.4",
        "build": 5,
        "package": {
          "url": "https://raw.githubusercontent.com/johndekroon/WhyOS-Apps/main/dist/v1/packages/gnuboy-1.0.4.wpk",
          "sha256": "…",
          "size": 214000,
          "installed_size": 420000
        }
      }
    }
  ]
}
```

- `terminal` is true for command-line apps (`launch.terminal` other than
  `false`, or only `[[commands]]`); `commands` lists the command names, so
  search finds `nmap` by its command and CI can check names are unique.
- `serial` increases with every publication that changes something. A
  client MUST refuse an index with a lower `serial` than the one it has
  cached (a stale mirror or cache). `expires` is optional and not written
  now.
- `install_location` and `menu_icon` (the menu icon named in
  `launch.icon`, the fallback when there is no `icon`) are in the index
  too, so lists need no details.
- `withdrawn` apps appear with `"withdrawn": "<reason>"` and no
  `latest`, so clients can offer removal.
- Relative paths (`icon`) are relative to `base_url`.

### 5.2 App details

`apps/<id>.json` holds what only the info page needs: `description`,
`homepage`, `source`, `license`, `bundled` (with links to the licence
texts, published next to the details), `permissions`, `storage`, `screenshots`
(list of paths), and `versions`: every published version with `version`,
`build`, `published`, `changelog` and its package (URL, SHA-256, size).
The client checks it against `details_sha256` in the index entry.
Bundled components carry `license_url` (a path under `licenses/`). Older versions stay downloadable, so a user can roll back by hand
(`store install gnuboy@1.0.3`); the client never downgrades on its own.

### 5.3 Images

The badge has 32 MB of PSRAM and a 720×720 RGB565 panel; decoding PNG
there means zlib plus filters for every image. CI therefore converts the
PNGs from the repository to **QOI** (a simple lossless format, a decoder
is about 100 lines of C) at the sizes the UI uses. The PNGs remain the
source; when the UI changes size, CI regenerates.

## 6. Integrity

There is no signature (decided 2026-10-12: publishing should be a plain
git push, without keys or a publishing job).

- The index and the details come over HTTPS from the source's base URL;
  the client checks TLS against the CA roots it carries.
- The index holds the SHA-256 of every package and detail file; the
  client refuses a package or details file that does not match, and every
  file it unpacks is checked against the manifest's `files.sha256`.
- `serial` stops a client from going back to an older index.
- What this does not protect against: someone who can push to this
  repository, or who controls a mirror the user added, can publish
  anything. Review and branch protection on `main` are the safeguard.

A signed index can be added later without breaking clients: an
`index.json.minisig` next to the index and a `KEYS` field in source
files, both ignored by clients that do not know them.
