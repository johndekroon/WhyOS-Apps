# Roadmap, open questions, non-goals

## Phases

| Phase | What | Done when |
|---|---|---|
| **1. Catalogue and CLI** | Schemas, `categories.json`, host tools (validate, pack, build index, sign), CI and Pages publishing; the `store` CLI on the badge; the runtime contract in `apprun`/`home`; the existing SD apps (gnuboy, NetSurf, Links, nmap, imgview) repackaged with `artifact.toml` | `store install gnuboy` works on a badge from a clean card, and remove leaves nothing behind |
| **2. Store UI and menu** | The full-screen UI, a store entry in the menu, grouping by subcategory, more than 24 apps, `store maintain` on card insertion, the store in the OS image | A user can browse, search, install, swap cards and reinstall without a shell |
| **3. Builds from source** | A container image with the badge toolchain for CI; `build.sh` per app; reproducible builds checked by building twice | New versions of the existing apps come from CI only |
| **Later** | Ratings or reactions (GitHub Discussions per app, read-only in the store); delta updates; enforcing `permissions` (needs OS support); app dependencies on shared data packs; localisation of store texts | as needed |

## Open questions

- **Flash budget for settings.** `/data` has about 2 MB free and holds
  `/etc`, `/root` and the store itself. Is a cap per app (default 16 KB,
  maximum 256 KB) enough, or does the store also need a total cap and a
  warning?
- **CA bundle.** Ship a small bundle with the store (only the roots for
  GitHub and known mirrors), or one shared bundle in the rootfs for every
  app? The rootfs is nearly full (why2025-linux `docs/DATA.md`).
- **Store binary size.** mbedTLS, Monocypher, inflate and the UI in flash:
  estimate about 1 MB. To be measured; if it is too large, the TLS part
  could become a shared helper program that NetSurf and Links use as
  well.
- **Hot-plug of the card.** Is there a reliable hook when a card is
  inserted while the badge runs, to run `store maintain`? Otherwise only
  at boot and when the store starts.
- **User folders and Files.** Should `appdata/<id>/<user folder>` show up
  in Files under a friendly name (e.g. "Game Boy ROMs"), so users find
  where to put files? Needs a small addition to the menu.
- **Apps outside `$HOME/.whyos`.** Existing apps write elsewhere
  (NetSurf: `/root/.netsurf`). Repackaging fixes the known ones; a check
  that lists unexpected writes after a test run would catch new ones.
- **Moderation.** Who besides the repository owner may merge, and is
  there a policy on what is accepted (licences, content)?
- **Signing key custody.** Only as a GitHub secret, or a hardware key and
  signing by a maintainer for each publication?

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
