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
  subcategory, search, an info page per app with screenshots, install,
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
  third-party stores), signed metadata, and room for permissions, ratings
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
| **Source** | A published catalogue: base URL plus signing key. The default source is this repository |
| **Package** | One file (`.wpk`) with one version of an app |
| **Manifest** | The app's metadata: identity, category, launch, requirements, storage. Authored as TOML, shipped as JSON |
| **Index** | The signed list of all apps in a source, built by CI |
| **Registry** | The record of installed apps in the home directory, one file per app |
| **Store client** | The `store` program on the badge: CLI and UI |
| **Type** | Top-level category: `app` or `game` (and `system`, hidden) |
| **Category** | Subcategory within a type, from [categories.json](../categories.json) |

The keywords MUST, SHOULD and MAY are used as in RFC 2119.

## How the parts fit together

```
 this repository (GitHub)                  published (GitHub)                    badge
 ─────────────────────────                 ──────────────────                    ─────
 apps/<id>/manifest.toml ─┐                Pages: /v1/index.json(.gz)            store client
 apps/<id>/icon.png       ├─► CI ─────────►       /v1/index.json.minisig ──────► verify signature
 apps/<id>/screenshots/   │   validate,           /v1/apps/<id>.json             cache index on SD
 apps/<id>/build.sh or    │   build/pack,         /v1/icons/<id>.qoi             search, browse
   prebuilt artifact ─────┘   sign                /v1/screens/<id>-<n>.qoi
                                            Releases: <id>-<ver>.wpk ───────────► download, check sha256,
                                                                                   install on SD card,
                                                                                   record in $HOME
```

1. An author adds or updates `apps/<id>/` through a pull request.
2. CI validates it, builds or fetches the package, and on merge publishes
   the package as a release asset and rebuilds the index.
3. CI signs the index. The index holds each package's SHA-256, so one
   signature covers everything a client downloads.
4. The client fetches the index over HTTPS, checks the signature with a
   key it already has, and installs packages into fixed places on the
   card and in the home directory ([device.md](device.md)).

## Versioning of this specification

Every file format carries a `format` number (manifest, index, registry,
categories, sources). Rules for all of them:

- A client MUST ignore fields it does not know.
- A client MUST refuse a document whose `format` is higher than it
  supports, and SHOULD tell the user to update the store.
- New optional fields do not change `format`. A change that old clients
  would misread does, and gets a new published path (`/v2/`), while `/v1/`
  stays published for old clients as long as practical.
