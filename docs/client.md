# The store client

## 1. One program, two faces

`store` is one static BFLT program with a command-line interface and a
full-screen UI. The UI calls the same functions as the CLI, so tests and
scripts exercise the code the user runs.

```
store                        the UI (what the menu starts)
store update                 fetch the index (and check its serial)
store search <words>         search the cached index
store info <id>              details, versions, state
store install <id>[@<ver>]   install or update
store remove <id>            remove everything the app owns
store list [--missing|--updates|--other]
store verify [<id>]          check installed files against .package/files.sha256
store repair <id>            reinstall the installed version
store adopt <id>             register an unregistered app on this card
store maintain               finish interrupted work, run pending cleanups (device.md §5.7)
store sources                list sources and their state
```

Exit status 0 on success; messages on stderr; `--json` on list commands
for scripts and tests.

## 2. Where it lives

In flash, as an app with `install_location = flash`
(`/data/apps/store/`, about 1 MB): it must work with any card, or none,
since reinstalling after a card swap is its main job. It updates itself
as the package `store` (type `system`): the new binary is unpacked next
to the running one and takes over on the next start. A copy of the
first version comes with the OS image (in `/data`'s initial contents),
so a fresh badge has a store without a computer.

## 3. Building blocks

| Need | How | Notes |
|---|---|---|
| HTTPS | mbedTLS 3.x, TLS 1.2 and 1.3 | Already built for NetSurf (why2025-linux `sdapps/build.sh`). Busybox `wget` on the badge has no TLS |
| CA certificates | Mozilla bundle, only the roots GitHub and the mirrors need, shipped with the store | Updated with the store. Full bundle stays in NetSurf/Links |
| Clock | Must be set before TLS (certificate validity) | `netd` sets it once after connecting; the store waits for it or says why it cannot connect |
| Redirects | Follow up to 5, HTTPS only | GitHub release downloads redirect |
| Resume | HTTP Range on `.part` files | Wi-Fi on the badge goes through the C6 and can drop |
| Integrity | SHA-256 of every package and detail file from the index (mbedTLS has SHA-256); `serial` against a stale index | No signature (repository.md §6) |
| Decompress | inflate (zlib or a small inflater such as `tinf`) | Streaming |
| Archive | ustar reader, streaming, with the checks of package.md §1.2 | ~300 lines |
| JSON | streaming tokenizer (jsmn-style) | The index is parsed in one pass into a compact in-memory table |
| Images | QOI decoder | Icons and screenshots are converted by CI (repository.md §5.3) |
| Drawing | The menu's drawing code and fonts (`overlay-src/wbdraw.h`, `home-assets`) and the design tokens of the badge's design system | Same look as the menu |

Memory budget: at most 1.5 MB of the user pool while the store runs
(the menu is out of the way, as for any app). Downloads and unpacking
stream to the card; nothing larger than a buffer is held in memory. The
index table for 1000 apps is about 200 KB (strings interned, no
descriptions). Screenshots load one at a time.

## 4. UI

Controlled with the keypad only, like the menu: arrows, Enter, Esc, the
symbol keys for actions, typing for search. English UI text.

| Screen | Contents |
|---|---|
| **Discover** | Updates waiting (if any), recently updated, featured (a list in the index, chosen by maintainers), and the types as tiles |
| **Apps / Games** | Subcategories of the type as a list with counts; a subcategory opens its apps sorted by name or recently updated |
| **Search** | Typing filters live over name, summary, tags, author and command names of the cached index, offline; results grouped by type |
| **App page** | Icon, name, author, version, size; a "Command-line" label and the commands it adds, for command-line apps; summary and description; screenshots (left/right); requirements and permissions; licence, with a "Licences" view listing the bundled components and their texts ("not open source" marked); storage it uses; changelog and older versions; state and buttons: Install / Open / Update / Remove / Reinstall |
| **My apps** | Sections in this order, each only when not empty: Updates (with "Update all"), Not on this card (with "Reinstall all"), Needs repair, Found on this card, Installed, Other apps (legacy), Pending cleanups |
| **Settings** | Sources (add a mirror, enable, disable), refresh interval, keep downloaded packages, clear cache, storage overview per app |

Progress (download, unpack) shows bytes and a bar; Esc cancels and
leaves nothing half-installed. Errors say what to do ("No network:
connect in Settings > Wi-Fi", "Card full: 12 MB needed, 3 MB free").

## 5. What the OS must provide

Changes in why2025-linux, outside this repository:

| Change | Where | Why |
|---|---|---|
| `APP_*` environment and `%D %C %T` placeholders | `apprun.c`, argument expansion in `home.c` | Runtime contract (device.md §6) |
| Rotation lock while an app runs | `apprun.c` (`ROTATION=locked` → `/tmp/rotation-lock`), `sleepd.c` (honour it) | Apps and games that use the accelerometer (package.md §2.3) |
| More than 24 apps | `MAX_APPS` in `home.c` | A store makes 24 too few; a dynamic list or a higher fixed limit |
| Read `ID`, `TYPE`, `SUBCATEGORY` from `app.conf` | `home.c` | Group the menu's lists by subcategory; open the store's page for an app |
| Store tile or entry in the menu | `home.c` | Start `store`; "My apps" also reachable from Settings |
| `$HOME/.whyos/bin` on `PATH` | `/etc/profile.d/whyos-apps.sh` in the rootfs overlay (login and interactive shells); `DEFAULT_PATH` in `dropbear-localoptions.h`, path written out (commands run directly over SSH) | Commands of command-line apps in every shell (package.md §2.3.1) |
| `/etc/whyos-release` | rootfs overlay | OS version and API level (device.md §6.3) |
| `store maintain` after the card is mounted | where the card is mounted (boot and hot-plug) | Finish interrupted work, pending cleanups |
| The first store binary in `/data` | `post-build.sh` / `mkdata.sh` | A fresh badge has a store |

None of these break existing hand-installed apps.

## 6. Testing

- Host build of the package checker and index parser (the same C), run
  in CI against good and malicious packages (path traversal, links,
  case collisions, oversized entries, wrong hashes).
- A local source: the index tools can publish to a directory; a test
  HTTP server on the build machine serves it to the badge or the QEMU
  simulation, with a test key.
- On the badge: install, update, card swap (missing → reinstall), remove
  with and without the card, power cut during install (journal), and
  memory use measured as for other apps.
