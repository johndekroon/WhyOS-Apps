# On the badge: layout, registry and lifecycle

Paths in this document use `$HOME` for the user's home directory (today
`/root`, in flash through the `/data` overlay) and `SD` for the card's
mount point (today `/mnt/sd`). The client MUST take both from the
environment and `/etc/whyos-release`, not hard-code them.

## 1. Where everything lives

```
SD/badge/apps/<id>/                program files of an installed app (replaced on update)
SD/badge/apps/<id>/app.conf        written by the store from launch (package.md §2.3)
SD/badge/apps/<id>/.package/       manifest.json, files.sha256, source: what was installed
SD/badge/appdata/<id>/             storage.data: large data, user folders (roms/, ...)
SD/badge/appdata/<id>/.cache/      storage.cache: may be deleted at any time
SD/badge/.store/card.json          this card's identity (§4)
SD/badge/.store/cache/             cached index, details and images
SD/badge/.store/download/          packages being downloaded (resumable)
SD/badge/apps/.<id>.new/           a package being unpacked (next to its place: same filesystem)
SD/badge/apps/.<id>.old/           the previous version while the new one moves in
SD/badge/.store/journal            the operation in progress (§5.7); $HOME/.whyos/store/journal without a card

/data/apps/<id>/                   program files of an app with install_location = flash
                                   (same contents as on the card, incl. app.conf and .package/)

$HOME/.whyos/config/<id>/          storage.config: small settings, survive a new card
$HOME/.whyos/bin/<command>         wrapper scripts for [[commands]] (package.md §2.3.1)
$HOME/.whyos/store/installed/<id>.json   registry: one file per installed app (§2.2)
$HOME/.whyos/store/cleanup.json    removals still to be done on cards that were absent (§5.4)
$HOME/.whyos/store/sources.d/      extra sources and overrides (§2.3)
$HOME/.whyos/store/settings.json   store preferences

/tmp/app-<id>.log, /tmp/app-<id>.ask, /tmp/app-bin/   run-time files of the menu (gone at reboot)
```

Why this split:

- **Card**: program and large data. Swapping the card swaps these.
- **`$HOME` (flash)**: what must outlive a card: the list of what you
  installed, and each app's small settings. Small enough for the few MB
  of `/data`.
- **Separate `apps/` and `appdata/`**: an update replaces `apps/<id>/` as
  a whole and never touches the data; the app directory never mixes
  shipped and user files.
- Everything under one prefix per app id in each place, so remove is a
  fixed list of directories, not a search.

`.whyos/` in `$HOME` keeps the store's and apps' files apart from
programs that use `~/.config` themselves.

## 2. The home directory

### 2.1 App settings

`$HOME/.whyos/config/<id>/` is created at install, empty, for apps that
declare `storage.config`. Apps find it in `APP_CONFIG` (§6). It is kept
on update, on reinstall after a card swap, and when the app is "missing";
it goes only on remove.

### 2.2 Registry

One JSON file per installed app, written atomically (write `<id>.json.new`,
`fsync`, rename), so a power cut never leaves a half-written registry and
JFFS2 only rewrites one small file per change.

```json
{
  "format": 1,
  "id": "gnuboy",
  "source": "whyos",
  "name": "Game Boy",
  "version": "1.0.4",
  "build": 5,
  "location": "sd",
  "cards": ["6f1c9a2e-3b7d-4e1a-9c55-0d2f8a7e4b11"],
  "installed": "2026-10-12T10:12:00Z",
  "updated": "2026-10-12T10:12:00Z",
  "pinned": false,
  "auto_installed": false
}
```

| Field | Meaning |
|---|---|
| `source` | Which source it came from (§2.3); with `id` the full identity |
| `name` | Copy for "My apps" when neither index nor card is available |
| `location` | `sd` or `flash` |
| `cards` | Ids of the cards that hold a copy (§4); usually one |
| `pinned` | The user chose to stay on this version: no update offers |
| `auto_installed` | Installed only as a dependency (`requires.apps`); offered for removal when nothing needs it anymore |

### 2.3 Sources

A source is a catalogue: id and base URL. The client has
the default source built in; files in `/etc/whyos/sources.d/` (system)
and `$HOME/.whyos/store/sources.d/` (user) add sources or override the
URL (a mirror), as `KEY=value` lines like the menu's other config files:

```
# $HOME/.whyos/store/sources.d/whyos-mirror.conf
ID=whyos
URL=https://mirror.example.org/whyos-apps/v1/
ENABLED=1
# CAFILE=/path/ca.pem   a test source with its own CA (client.md §6)
```

Without signatures (repository.md §6) a source is exactly as trustworthy
as whoever runs it: add a mirror only from someone you trust.

App ids are unique per source. When two sources offer the same id, the
UI shows both with the source name; the registry records which was
installed. Updates only come from the source an app was installed from.

## 3. The app directory

The store writes, next to the package's `files/`:

- `app.conf`: generated from the manifest (package.md §2.3). Users and
  apps MUST NOT edit it; it is regenerated on update.
- `.package/manifest.json`: the manifest as installed.
- `.package/files.sha256`: hash per file, for `store verify` and repair.
- `.package/source`: source id and the URL the package came from.

`.package/` is the marker that the store owns this directory. A directory
in `badge/apps/` without it is a *legacy* app (§5.8).

## 4. Card identity

The first time the store sees a card, it writes
`SD/badge/.store/card.json`:

```json
{ "format": 1, "card": "6f1c9a2e-3b7d-4e1a-9c55-0d2f8a7e4b11", "created": "2026-10-12T10:00:00Z" }
```

A random UUID, not the FAT volume serial: formatting a card on a computer
gives it a new serial, and two cards can share one after cloning. The
registry's `cards` list tells "missing" (the card that held it is gone)
apart from "on another card" (that card exists, it just is not
inserted).

## 5. Lifecycle

### 5.1 States

The client derives an app's state each time from three things: the
registry, the card in the slot, and the index.

| State | Registry | Files in place | Shown in My apps as | Actions |
|---|---|---|---|---|
| installed | yes | yes, hashes OK | Installed | Open, Update (if newer), Remove |
| update available | yes | yes | Updates | Update, Pin, Remove |
| missing | yes | no; current card is not in `cards` or no card | Not on this card | Reinstall, Remove |
| damaged | yes | partly, or hashes differ | Needs repair | Repair (reinstall same version), Remove |
| unregistered | no | yes, with `.package/` | Found on this card | Adopt (create registry entry), Delete |
| legacy | no | `app.conf` without `.package/` | Other apps | Open, Delete, Replace with store version (if the id exists in the index) |
| withdrawn | yes | any | Withdrawn, with reason | Remove (Open while files are there) |

"Missing" is not an error: it is the normal result of swapping the card,
and the reason the registry lives in flash.

### 5.2 Install

1. Refresh the index if it is older than the configured age (default 1
   hour) and the network is up; otherwise use the cached one.
2. Resolve: take the latest version's package, check
   `requires.os_api`, `requires.features`, `requires.apps` (install those
   first), and warn on `requires.memory_kb`.
3. Check space: on the card `size` + `installed_size` + 10 %, and in
   flash the declared `storage.config.max_kb` (and the package, for
   `flash`).
4. Write the journal: `install <id> <build>`.
5. Download to `.store/download/<file>.part` (resume with HTTP Range),
   check size and SHA-256 against the index, rename to `<file>`.
6. Unpack into `apps/.<id>.new/` (for `flash`: `/data/apps/.<id>.new/`,
   so moving it in is a rename on one filesystem): `manifest.json` first, check it
   matches the index entry (id, version, build), then `files/`, checking
   each file against `files.sha256` while streaming.
7. Write `app.conf` and `.package/` into `.<id>.new/`.
8. Move into place: `apps/<id>` (if present) → `apps/.<id>.old`, `.<id>.new` →
   `apps/<id>`. On FAT a directory rename is one directory-entry change.
   Then the command wrappers (package.md §2.3.1) into `$HOME/.whyos/bin/`.
9. Create `appdata/<id>/` with its `folders`, and `$HOME/.whyos/config/<id>/`.
10. Write the registry entry; add this card to `cards`.
11. Run `hooks.post_install`; delete `apps/.<id>.old`, the download (unless
    "keep packages" is on), and clear the journal.

### 5.3 Update

The same steps, with `migrate` instead of `post_install`. `appdata/<id>/`
and the config directory stay as they are. The client never installs a
lower `build` than the registry's, except on an explicit
`store install <id>@<version>`, which asks first. Pinned apps are not
offered updates.

### 5.4 Remove

Remove deletes everything the app owns, in this order:

1. `hooks.pre_remove`, if the files are there.
2. With `user_folders`: show what they hold (number of files, size) and
   ask for confirmation once, naming them ("12 files in roms, 3.1 MB").
3. Journal: `remove <id>`.
4. Delete `SD/badge/apps/<id>/` or `/data/apps/<id>/`,
   `SD/badge/appdata/<id>/` (cache and user folders included),
   `$HOME/.whyos/config/<id>/`, and its command wrappers in
   `$HOME/.whyos/bin/`.
5. Delete the menu's run-time files for it: `/tmp/app-<id>.log`,
   `/tmp/app-<id>.ask`, `/tmp/app-bin/<exec name>`.
6. Delete the registry entry; clear the journal.

**When the card is not the one that holds it** (state *missing*), or a
copy lives on other cards: steps 4–6 run for flash and `$HOME`, and the
card parts are recorded in `$HOME/.whyos/store/cleanup.json`:

```json
{ "format": 1, "pending": [ { "card": "6f1c…", "id": "gnuboy", "paths": ["badge/apps/gnuboy", "badge/appdata/gnuboy"] } ] }
```

When a card with that id is inserted, `store maintain` (§5.7) deletes
those paths, but only if `apps/<id>/.package/` there still names the same
source and id, so a card reused for something else is not damaged.
"My apps" shows pending cleanups and lets the user drop them.

### 5.5 Reinstall after a card swap

"Reinstall" on a *missing* app installs the version in the index (or the
registry's version, if pinned and still published), keeps the config
directory, creates an empty `appdata/<id>/`, and adds the new card to
`cards`. "Reinstall all missing" does this for every missing app in one
go, with one confirmation listing the total download size.

### 5.6 Adopt

A card from another badge, or a badge whose flash was reflashed, has
apps with `.package/` that the registry does not know. "Adopt" creates
the registry entry from `.package/manifest.json` and `.package/source`
(after `verify`), and an empty config directory if there is none.

### 5.7 Interruptions and maintenance

Power cuts, a pulled card, or the watchdog can stop the store anywhere.
`.store/journal` holds the operation in progress. `store maintain`, run
by the client at start and by the system after the card is mounted
(boot, and on insertion):

- an unfinished **install/update**: if `apps/<id>` is complete (its
  `.package/files.sha256` matches), finish steps 9–11; otherwise restore
  `apps/.<id>.old` if present, else remove the half-made directory and
  mark the registry entry *damaged* (if there is one);
- an unfinished **remove**: finish it;
- deletes `.<id>.new/` directories, puts back an `.<id>.old/` whose
  `<id>` is gone and deletes the others, and `download/` parts older than
  7 days;
- runs pending cleanups for this card (§5.4);
- makes the programs in installed apps runnable (package.md §1.2): store
  0.2.10 and older left programs that the manifest does not name `0644`,
  which do not run from an f2fs card.

### 5.8 Legacy apps

Directories in `badge/apps/` or `/data/apps/` with an `app.conf` but no
`.package/` were copied by hand. The menu keeps listing them; the store
shows them under "Other apps" and never changes them on its own. "Delete"
removes only that directory (the store cannot know what else such an app
wrote). "Replace with store version" installs the store's package over it
after a confirmation; the old directory is moved to `apps/.<id>.old`
first and deleted after success.

## 6. What an app may rely on (runtime contract)

### 6.1 Environment

`apprun` sets, for every app started from the menu (store-installed or
not):

| Variable | Value |
|---|---|
| `APP_ID` | the app id (for legacy apps: the directory name) |
| `APP_DIR` | the app directory (read-only by convention) |
| `APP_DATA` | `SD/badge/appdata/<id>` (created if missing) |
| `APP_CONFIG` | `$HOME/.whyos/config/<id>` (created if missing) |
| `APP_CACHE` | `SD/badge/appdata/<id>/.cache`, or `/tmp/cache/<id>` without a card |
| `WHYOS_API` | the OS API level |

With `ROTATION=locked` in `app.conf`, `apprun` creates
`/tmp/rotation-lock` holding its own pid before it starts the app, and
removes it when the app ends. `sleepd` checks for that file on each
accelerometer tick: while it exists and the pid is alive, it puts the
panel at rotation 0, restores the normal arrow mapping and stops
polling the accelerometer. A stale file (pid gone, e.g. after a crash)
is ignored and deleted, so rotation can never stay locked by accident.

The command wrappers in `$HOME/.whyos/bin/` set the same variables, so
an app behaves the same from the menu and from a shell.

The placeholders `%D`, `%C` and `%T` in `ARGS` and `FILES` expand to the
same values ([package.md](package.md) §2.3). This is a small change in
the menu's argument expansion and in `apprun`; the existing `%d`, `%f`,
`%u`, `%a` stay.

### 6.2 Rules for apps

- Write only to `APP_DATA`, `APP_CONFIG`, `APP_CACHE` and `/tmp`, plus
  files the user explicitly chose (an "export to" dialog). Anything else
  survives removal, which this store promises not to happen.
- Keep `APP_CONFIG` small (its declared `max_kb`): it is flash, shared by
  every app and the system, and wears with every write. Write settings
  when they change, not periodically.
- Work without a card when possible (`APP_DATA` may be missing if the
  card is removed while running); fail with a non-zero exit status and a
  clear message otherwise.
- Follow why2025-linux `docs/APPS.md` for the display, keypad, memory and
  exit status.

Porting existing apps: gnuboy's `--savedir=/mnt/sd/roms/saves` becomes
`%D/roms/saves` and `FILES=/mnt/sd/roms` becomes `%D/roms`; NetSurf's
cookies and history move from `/root/.netsurf` to `APP_CONFIG` (by
starting it with `HOME=$APP_CONFIG`, or a patch).

### 6.3 `/etc/whyos-release`

A `KEY=value` file in the rootfs that tells the store which OS version and API level it runs on:

```
NAME=WhyOS
VERSION=2026.10
API=1
FEATURES="wifi bluetooth usb-host sd-card display keyboard"
SD=/mnt/sd
```

`API` goes up when the OS adds something apps may rely on (a new
placeholder, a new device, a library); it never goes down. A changelog of
API levels lives in why2025-linux `docs/APPS.md`.

## 7. Space

- The store SHOULD show, per app, its size in `apps/`, `appdata/` (user
  folders separately) and `config/`, and the free space on card and
  flash.
- When the card runs low, it offers to clear app caches and `.store/cache/`.
- When an app's config is over its `max_kb`, "My apps" marks it; the
  store does not truncate app files.
