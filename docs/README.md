# WhyOS app store: overview

Spec version **1.0-draft**, 2026-10-12.

The badge runs Linux ("WhyOS" in this document). Apps already run from the
microSD card: a directory with an `app.conf` in `badge/apps/<app>/`, which
the start menu (`home`) lists and starts through `apprun`
(why2025-linux `docs/APPS.md`). Installing means copying directories by
hand, and there is no catalogue, no versions, no updates and no clean way
to remove an app with everything it wrote. This specification adds those,
backed by a GitHub repository.

## Goals

- **Store experience on the badge:** browse by type (app or game) and
  category (at most four per type), search, an info page per app with screenshots, install,
  update, open, remove.
- **Install to the microSD card.** The badge's flash is small (a few MB
  free) and only holds small settings and the store itself.
- **Settings survive a card swap.** An app's small settings live in the
  home directory (flash); its program and large data on the card. Put in
  a new card, reinstall, and the settings are still there.
- **"My apps".** The home directory records what is installed. Apps whose
  files are gone (new card) can be reinstalled or removed for good.
- **Remove means everything:** program, data on the card, settings in
  flash, caches, logs and the registry entry.
- **Future-proof:** versioned formats, several sources (mirrors,
  third-party stores), checksummed metadata, and room for permissions, ratings
  and source builds without breaking old clients. Only WhyOS on the badge
  is targeted; there are no other platforms.

## Non-goals (for now)

Payments, user accounts, ratings and reviews, sandboxing, automatic
background updates, shared libraries between apps. See
[roadmap.md](roadmap.md).

## Terms

| Term | Meaning |
|---|---|
| **App id** | Unique, permanent name of an app in a source: `[a-z0-9][a-z0-9-]{1,31}`, e.g. `gnuboy` |
| **Source** | A published catalogue: a base URL. The default source is this repository's `dist/v1/` |
| **Package** | One file (`.wpk`) with one version of an app |
| **Manifest** | The app's metadata: identity, category, launch, requirements, storage. Authored as TOML, shipped as JSON |
| **Index** | The list of all apps in a source, with the SHA-256 of every package, built by `tools/publish.py` |
| **Registry** | The record of installed apps in the home directory, one file per app |
| **Store client** | The `store` program on the badge: CLI and UI |
| **Type** | Top-level category: `app` or `game` (and `system`, hidden) |
| **Category** | Subcategory within a type, from [categories.json](../categories.json) |

The keywords MUST, SHOULD and MAY are used as in RFC 2119.

## How the parts fit together

```
 this repository (GitHub)                                                         badge
 ─────────────────────────                                                        ─────
 apps/<id>/manifest.toml ─┐                    dist/v1/index.json(.gz)            store client
 apps/<id>/icon.png       ├─► tools/publish.py ─► dist/v1/apps/<id>.json ─ git ─► fetch over HTTPS
 apps/<id>/screenshots/   │   validate, pack,     dist/v1/packages/*.wpk    push   (raw.githubusercontent.com),
 apps/<id>/artifact.toml ─┘   hash, convert       dist/v1/icons/, screens/         check SHA-256, install
                                                                                    on SD, record in $HOME
```

1. An author adds or updates `apps/<id>/` through a pull request.
2. A maintainer runs `tools/publish.py`, which validates the apps, packs
   each new version into `dist/v1/packages/` and rewrites the index and the
   detail files, and commits `apps/` and `dist/` together.
3. The push publishes: the badge reads `dist/v1/` from the repository over
   HTTPS. The index holds the SHA-256 of every package and detail file.
4. The client fetches the index over HTTPS (TLS checked against the CA
   roots it carries), checks each package against the index's SHA-256, and
   installs it into fixed places on the card and in the home directory
   ([device.md](device.md)).

There is no signature: what the badge installs is as trustworthy as the
repository and TLS to GitHub (repository.md §6).

## Versioning of this specification

Every file format carries a `format` number (manifest, index, registry,
categories, sources). Rules for all of them:

- A client MUST ignore fields it does not know.
- A client MUST refuse a document whose `format` is higher than it
  supports, and SHOULD tell the user to update the store.
- New optional fields do not change `format`. A change that old clients
  would misread does, and gets a new published path (`/v2/`), while `/v1/`
  stays published for old clients as long as practical.
