# This repository: catalogue, CI and publishing

## 1. Layout

```
apps/<id>/manifest.toml        the manifest (package.md §2)
apps/<id>/icon.png             256×256 PNG with transparency
apps/<id>/screenshots/<n>.png  up to 6, 720×720 PNG (the panel), named 1.png, 2.png, ...
apps/<id>/build.sh             EITHER: builds the package's files/ from pinned sources
apps/<id>/artifact.toml        OR: a prebuilt files/ archive: URL + SHA-256
apps/<id>/README.md            optional notes for maintainers (not shown in the store)
categories.json                the category tree (§3)
keys/<key id>.pub              public signing keys (minisign format)
schema/                        JSON Schemas
tools/                         validator, packer, index builder (host tools, any OS)
docs/                          this specification
.github/workflows/             CI
```

- One directory per app id. The directory name MUST equal `id`.
- A removed app keeps a tombstone: `apps/<id>/manifest.toml` with
  `withdrawn = "<reason>"`. Its id is never reused, and clients offer
  removal to anyone who has it installed.

### 1.1 Prebuilt or built from source

Two ways to supply the program:

- **`artifact.toml`** (phase 1): the author builds the app with the badge
  toolchain and publishes a `files/` archive (tar.gz) somewhere stable,
  for example a release in their own repository. `artifact.toml` pins the
  URL and SHA-256. CI downloads it, checks the hash and
  packs it.
- **`build.sh`** (phase 3): CI builds from pinned sources in a container
  with the badge toolchain ([roadmap.md](roadmap.md)). The script gets
  `$OUT` (the `files/` directory to fill) and the toolchain
  in `$PATH`; it MUST pin every download by version and SHA-256.

```toml
# apps/gnuboy/artifact.toml
url = "https://github.com/example/gnuboy-badge/releases/download/v1.0.4/files.tar.gz"
sha256 = "…"
```

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
so the store's tabs stay tidy.

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

- `id`s are permanent. A category can be renamed (`name`) freely; to
  retire one, mark it `"hidden": true` and move its apps.
- `menu` says where the start menu lists apps of this type or category
  by default (`apps`, `games`, `tools`, `none`); a category may override
  its type's.
- `icon` is one of the menu's built-in icon names.
- Each type has a category `other` as a fallback.

## 4. CI

On every pull request:

1. Validate each changed `manifest.toml` against the schema (after
   conversion to JSON) and against `categories.json`.
2. Check ids: directory name, uniqueness, no reuse of withdrawn ids;
   `build` higher than the published one.
3. Fetch or build `files/`, apply the entry rules of
   [package.md](package.md) §1.2, compute sizes and hashes, check
   `launch.exec` exists, and check BFLT binaries (header, stack size, no
   absolute `jalr` into the first 2 KB: the same check as why2025-linux
   `sdapps/build.sh`).
4. Pack the `.wpk` and run the client's own package checker on it (the
   same code, built for the host).
5. Convert icon and screenshots (§5.3), check their sizes.

On merge to `main`, additionally:

6. Create a release per new app version, tag `<id>/v<version>`, with the
   `.wpk` files as assets.
7. Rebuild the index and detail files, sign the index, publish to Pages.

The signing key is a repository secret available only to the publish
job on `main`.

## 5. Published files

Base URL of the default source: `https://johndekroon.github.io/WhyOS-Apps/v1/`.

```
v1/index.json            the index (§5.1)
v1/index.json.gz         the same, gzip; clients SHOULD fetch this one
v1/index.json.minisig    signature over index.json (uncompressed)
v1/apps/<id>.json        details per app (§5.2)
v1/icons/<id>.qoi        icon, 96×96, for lists and the info page
v1/icons/<id>@2.qoi      icon, 192×192, for a featured banner
v1/screens/<id>-<n>.qoi  screenshot, 360×360 (half size, fits the info page)
v1/categories.json       copy of the category tree
```

Packages are release assets:
`https://github.com/johndekroon/WhyOS-Apps/releases/download/<id>%2Fv<version>/<file>.wpk`.
GitHub answers with a redirect to `objects.githubusercontent.com`; clients
MUST follow redirects (HTTPS only). URLs in the index are absolute, so a
mirror can host packages elsewhere.

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
  "expires": "2026-11-12T10:00:00Z",
  "base_url": "https://johndekroon.github.io/WhyOS-Apps/v1/",
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
          "url": "https://github.com/johndekroon/WhyOS-Apps/releases/download/gnuboy%2Fv1.0.4/gnuboy-1.0.4.wpk",
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
- `serial` increases with every publication. A client MUST refuse an
  index with a lower `serial` than the one it has cached (rollback
  protection), and SHOULD warn when `expires` has passed (a stale mirror
  or a frozen index).
- `withdrawn` apps appear with `"withdrawn": "<reason>"` and no
  `latest`, so clients can offer removal.
- Relative paths (`icon`) are relative to `base_url`.

### 5.2 App details

`apps/<id>.json` holds what only the info page needs: `description`,
`homepage`, `source`, `license`, `permissions`, `storage`, `screenshots`
(list of paths), and `versions`: every published version with `version`,
`build`, `published`, `changelog` and its package (URL, SHA-256, size).
It is covered by the signature through `details_sha256` in the index
entry. Older versions stay downloadable, so a user can roll back by hand
(`store install gnuboy@1.0.3`); the client never downgrades on its own.

### 5.3 Images

The badge has 32 MB of PSRAM and a 720×720 RGB565 panel; decoding PNG
there means zlib plus filters for every image. CI therefore converts the
PNGs from the repository to **QOI** (a simple lossless format, a decoder
is about 100 lines of C) at the sizes the UI uses. The PNGs remain the
source; when the UI changes size, CI regenerates.

## 6. Signing

- Format: **minisign** (Ed25519). CI signs with `minisign -S`; the badge
  verifies with Monocypher. A minisign key id in every signature allows
  several keys and key rotation.
- Public keys live in `keys/` and are compiled into the client's default
  `sources.d` ([device.md](device.md) §2.3).
- Rotation: publish the new key in `keys/`, ship a client that trusts
  both, sign with the new key once enough clients have it, then drop the
  old one. A compromised key is handled the same way, faster, plus a
  store release that distrusts it.
- What is signed: `index.json`. The index contains the SHA-256 of every
  package and of every detail file; images are not signed (they are only
  displayed, never executed).
- TLS protects the download path; the signature protects the content,
  including when packages come from a mirror.
