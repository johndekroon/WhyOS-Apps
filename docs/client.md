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
store delete <id>            delete a legacy or unregistered app's directory
store pin <id> | unpin <id>  stay on the installed version, or not
store check-package <file> [<sha256>]   the package checks alone (CI, tests)
```

Options anywhere: `--yes` (answer yes: confirmations such as user folders
on remove or an explicit downgrade), `--json`, `--keep` (keep the downloaded
package). Exit status 2 when a confirmation was needed and not given.

Exit status 0 on success; messages on stderr; `--json` on list commands
for scripts and tests.

## 2. Where it lives

In flash, as an app with `install_location = flash`
(`/data/apps/store/`, 905 KB): it must work with any card, or none,
since reinstalling after a card swap is its main job. It updates itself
as the package `store` (type `system`, [apps/store](../apps/store)): the new
version is unpacked next to the running one and moved into place; on NOMMU
the running program is in RAM, so the new one takes over on the next
start. A copy of the first version comes with the OS image (in `/data`'s
initial contents, why2025-linux `stage-store.py`, pinned by commit and
SHA-256) with its `.package/` but no registry entry; `store maintain` at the
first boot registers it (adopt), so a fresh badge has a store without a
computer.

## 3. Building blocks

| Need | How | Notes |
|---|---|---|
| HTTPS | mbedTLS 3.x, TLS 1.2 and 1.3 | Already built for NetSurf (why2025-linux `sdapps/build.sh`). Busybox `wget` on the badge has no TLS |
| CA certificates | 12 roots from Mozilla's list (`client/roots.pem`, picked by `mkroots.py`): the ones GitHub and the usual hosts need, built into the store | Updated with the store. Full bundle stays in NetSurf/Links. A source can add its own (`CAFILE=`, for test sources) |
| Clock | Must be set before TLS (certificate validity) | `netd` sets it once after connecting; the store waits for it or says why it cannot connect |
| Redirects | Follow up to 5, HTTPS only | GitHub release downloads redirect |
| Resume | HTTP Range on `.part` files | Wi-Fi on the badge goes through the C6 and can drop |
| Integrity | SHA-256 of every package and detail file from the index (mbedTLS has SHA-256); `serial` against a stale index | No signature (repository.md §6) |
| Decompress | zlib's inflate | Streaming |
| Archive | ustar reader, streaming, with the checks of package.md §1.2 | ~300 lines |
| JSON | jsmn | The index is parsed in one pass into a compact in-memory table |
| Images | QOI decoder (`client/src/qoi.c`) | Icons and screenshots are converted by `tools/publish.py` (repository.md §5.3), fetched when a page opens and cached |
| Drawing | The menu's drawing code and fonts (why2025-linux `overlay-src/wbdraw.h`, `home_assets.h`) and the design tokens of the badge's design system | Same look as the menu. Drawn in bands of 40 rows, as appterm does: no 1 MB frame buffer |

Memory budget: at most 1.5 MB of the user pool while the store runs
(the menu is out of the way, as for any app). Downloads and unpacking
stream to the card; nothing larger than a buffer is held in memory. The
index table for 1000 apps is about 200 KB (strings interned, no
descriptions). Screenshots load one at a time; an app page keeps its
cover (180 x 180 px, 65 KB; repository.md §5.3) for its header. Measured on the badge
(2026-10-12): 920 KB at the peak of an install from the CLI, 1.3 MB for
the UI.

## 4. UI

Controlled with the keypad only, like the menu: arrows, Enter, Esc, the
symbol keys, typing for search. English UI text. The keys follow the
WhyBadge design (its Docs/06-design-system.md 6.6-6.8; in the source
since store 0.2.5, 2026-10-18): Enter does the row's
own thing (a switch flips; the arrows never change one), Esc goes back
one screen and closes the store from the first one (in Search it first
clears what was typed), Backspace only erases text. ■ opens a
category's menu (sort by name or recently updated); ● the store's log
(the end of apprun's `/tmp/app-store.log`, "No log yet." without one);
♣ Settings; ◆ a help pop-up with the screen's keys. The hint bar shows
only Enter, ■ where a screen has it and Esc with its word (close, back,
clear).

| Screen | Contents |
|---|---|
| **Discover** | Updates waiting (if any), recently updated, featured (a list in the index, chosen by maintainers), and the types as tiles |
| **Apps / Games** | "All" (every app of the type in one list), then the subcategories as a list with counts; each opens its apps sorted by name or recently updated |
| **Search** | Typing filters live over name, summary, tags, author and command names of the cached index, offline; results grouped by type |
| **App page** | The cover at 180 x 180 px (a crop or the first screenshot; the icon when there is none), name, author, version, size; a "Command-line" label and the commands it adds, for command-line apps; summary and description; screenshots (left/right); requirements and permissions; licence, with a "Licences" view listing the bundled components and their texts ("not open source" marked); storage it uses; changelog and older versions; state and buttons: Install / Open / Update / Remove / Reinstall |
| **My apps** | Sections in this order, each only when not empty: Updates (with "Update all"), Not on this card (with "Reinstall all"), Needs repair, Found on this card, Installed, Other apps (legacy), Pending cleanups |
| **Settings** | Sources (add a mirror, enable, disable), refresh interval, keep downloaded packages, clear cache, storage overview per app |

Progress (download, unpack) shows bytes and a bar; Esc cancels and
leaves nothing half-installed. Errors say what to do ("No network:
connect in Settings > Wi-Fi", "Card full: 12 MB needed, 3 MB free").

As built (2026-10-12, `client/src/ui.c`):

- Discover shows the types as list rows with counts, like the menu's
  lists. Featured comes from `featured` in the index (none yet).
- Questions the install asks (replacing a hand-installed app, a big app
  in flash, a downgrade) come up as dialogs with Cancel focused. Remove
  and delete ask first.
- Esc cancels a download. The unpack after it is not cancelled (it is
  short, and the journal covers an interruption). The partial download
  stays in `badge/.store/download/` for resuming, until "Clear cache".
- "Open" writes the app's id to `/tmp/store-open` and ends; the menu,
  back from apprun, starts the app, or shows its files first when it has
  `FILES` (why2025-linux `home.c`).
- An app whose `app.conf` has `CATEGORY="none"` (`menu = "none"`, e.g.
  the Picture viewer) only starts by opening a file: its page has no
  "Open" (since store 0.2.6, 2026-10-19). Open started it without a
  file, and it quit at once.
- Settings has no refresh interval: the index is refreshed when older
  than an hour (`INDEX_MAX_AGE`), and with "Refresh now". A mirror is a
  new address for an existing source (`$HOME/.whyos/store/sources.d/<id>.conf`).
- `STORE_UI_TIMING=1` writes the drawing time of each screen to stderr.

## 5. What the OS must provide

Changes in why2025-linux, outside this repository (all done on its
`usb-host` branch, 2026-10-12; that branch is not public):

| Change | Where | Why |
|---|---|---|
| `APP_*` environment and `%D %C %T` placeholders | `apprun.c`, argument expansion in `home.c` | Runtime contract (device.md §6) |
| Rotation lock while an app runs | `apprun.c` (`ROTATION=locked` → `/tmp/rotation-lock`), `sleepd.c` (honour it) | Apps and games that use the accelerometer (package.md §2.3) |
| More than 24 apps | `MAX_APPS` in `home.c` | A store makes 24 too few; a dynamic list or a higher fixed limit |
| Read `ID`, `TYPE`, `SUBCATEGORY` from `app.conf` | `home.c` | Open the store's page for an app. The menu does not group by subcategory (2026-10-13): categories are for browsing the store |
| Store tile or entry in the menu | `home.c` | Start `store`; "My apps" also reachable from Settings |
| `$HOME/.whyos/bin` on `PATH` | `/etc/profile.d/whyos-apps.sh` in the rootfs overlay (login and interactive shells); `DEFAULT_PATH` in `dropbear-localoptions.h`, path written out (commands run directly over SSH) | Commands of command-line apps in every shell (package.md §2.3.1) |
| `/etc/whyos-release` | rootfs overlay | OS version and API level (device.md §6.3) |
| `store maintain` after the card is mounted | `S15sdcard` at boot (with `HOME=/root`), and the menu's Settings > Storage & devices > Disks when it mounts the card. There is no automount when a card is inserted while the badge runs | Finish interrupted work, pending cleanups |
| The first store in `/data` | `post-build.sh` runs `stage-store.py` | A fresh badge has a store |
| "Open" from the store | `home.c` reads `/tmp/store-open` when the store ends | The store cannot start apps itself: apprun and the menu do that |

None of these break existing hand-installed apps.

## 6. Testing

- Host build of the package checker and index parser (the same C), run
  in CI against good and malicious packages (path traversal, links,
  case collisions, oversized entries, wrong hashes).
- A local source: `tools/publish.py --out` publishes to a directory and
  `tools/serve.py` serves it over HTTPS with a test CA (`CAFILE=` in the
  source), to the host tests, the badge or the QEMU simulation
  (why2025-linux `Tools/usb/sim/test_s10_store.py`).
- The screens on the host: `store-ui-host` takes keys from a script and
  writes screens as PPM (`client/tests/test_ui.py`).
- On the badge: install, update, card swap (missing → reinstall), remove
  with and without the card, power cut during install (journal), and
  memory use measured as for other apps.
