# Packages and manifests

## 1. Package file

A package is one version of an app:

```
<id>-<version>.wpk        gnuboy-1.0.4.wpk
```

It is a **gzip-compressed POSIX ustar archive** (`tar --format=ustar -cz`).
Standard tools create it; the badge has no `tar`, so the client carries a
small streaming ustar reader and inflater ([client.md](client.md)). The
extension is `.wpk` so the file is not mistaken for a general archive.

### 1.1 Layout

```
manifest.json          MUST be the first entry
files/                 the app directory: becomes badge/apps/<id>/ (or /data/apps/<id>/)
files/<exec>           the program named in launch.exec
files/...              anything else the app needs at run time
```

- `manifest.json` first lets the client check the package before it
  writes anything.
- Everything outside `manifest.json` and `files/` is reserved; clients
  MUST ignore it. (Room for later additions such as `migrate/` or
  `delta/`.)
- The client writes `app.conf` and `.package/` into the app directory
  itself ([device.md](device.md) §3). A package MUST NOT contain
  `files/app.conf` or `files/.package/`.

### 1.2 Entry rules

The badge's card is FAT, and the client runs as root without any
sandbox, so the archive is checked strictly. CI rejects, and the client
MUST refuse, a package with:

- an absolute path, a `..` component, or a path outside the layout above;
- an entry that is not a regular file or a directory (no symlinks, hard
  links, devices, FIFOs);
- a name component longer than 100 bytes, a whole path over 240 bytes, or
  characters FAT cannot store (`"*:<>?\|`, control characters), or a
  trailing dot or space;
- two paths that differ only in case;
- more than 4096 entries, or an unpacked size over 256 MB;
- a file whose size or SHA-256 differs from `files.sha256` in the
  manifest (see 2.9).

Unix permissions in the archive are ignored: on FAT every file is
executable, and on `/data` the client sets `0755` on `launch.exec` and
`0644` on the rest.

## 2. Manifest

Authors write `apps/<id>/manifest.toml` in this repository. CI validates
it, adds generated fields (sizes, hashes, publish dates) and writes
`manifest.json` into the package and the index. The badge only reads
JSON. The schema is [schema/manifest.schema.json](../schema/manifest.schema.json);
this section explains the fields. Real manifests:
[apps/gnuboy/manifest.toml](../apps/gnuboy/manifest.toml) (a game
with a ROM folder), [apps/nmap/manifest.toml](../apps/nmap/manifest.toml)
(a command-line app) and [apps/netsurf/manifest.toml](../apps/netsurf/manifest.toml)
(many bundled libraries). In TOML, top-level keys (`tags`, `permissions`, ...)
must come before the first `[table]`, or they become part of that table;
the strict schema reports this as an unknown key.

### 2.1 Identity

| Field | Req. | Meaning |
|---|---|---|
| `format` | yes | Manifest format, `1` |
| `id` | yes | App id, `[a-z0-9][a-z0-9-]{1,31}`. Permanent: never renamed, never reused for another app |
| `name` | yes | Display name, ≤ 32 characters |
| `version` | yes | SemVer 2.0 (`1.0.4`, `2.0.0-beta.1`) |
| `build` | yes | Integer, strictly increasing over all versions of this app. Orders versions and blocks downgrades without parsing SemVer |
| `summary` | yes | One line, ≤ 80 characters, shown in lists |
| `description` | yes | Plain text; blank lines separate paragraphs; ≤ 4000 characters |
| `author` | yes | Name of the author or team |
| `maintainer` | no | GitHub username responsible in this repository |
| `license` | yes | SPDX expression for the package as a whole: the app and everything bundled with it (§2.10) |
| `homepage` | no | URL |
| `source` | yes* | Where the corresponding source is (§2.10). *Required for every package with native code |
| `changelog` | no | Plain text for this version, ≤ 2000 characters |
| `upstream_version` | no | Version of the upstream program, if it differs (e.g. nmap `7.95`) |

### 2.2 Classification

| Field | Req. | Meaning |
|---|---|---|
| `type` | yes | `app`, `game` or `system`. `system` packages (the store itself, later drivers or fonts) are not shown in the browse tabs |
| `category` | yes | A category id that exists under this type in [categories.json](../categories.json) |
| `tags` | no | Up to 8 lowercase words for search (`emulator`, `gameboy`) |
| `content_warning` | no | Free text shown on the info page (e.g. flashing images) |

### 2.3 Launch

The `[launch]` table holds what `app.conf` holds today
(why2025-linux `docs/APPS.md`). At install time the client writes
`app.conf` from it, so the start menu and `apprun` need no knowledge of
the store. Keys map one to one:

| Manifest | app.conf | Notes |
|---|---|---|
| `exec` (required) | `EXEC` | Relative to the app directory |
| `args` | `ARGS` | Placeholders below |
| `info` | `INFO` | Defaults to `summary` cut to fit |
| `icon` | `ICON` | One of the menu's built-in icon names |
| `terminal` | `TERMINAL` | `false`, `true`, `"close"`, `"shell"` |
| `esc` | `ESC` | `"quit"` |
| `files`, `types` | `FILES`, `TYPES` | `files` MUST use a placeholder (`%D/roms`), never a fixed path |
| `opens`, `opens_also` | `OPENS`, `OPENS_ALSO` | Lists of extensions |
| `ask`, `ask_default` | `ASK`, `ASK_DEFAULT` | |
| `rotation` | `ROTATION` | `"auto"` (default) or `"locked"`; see below |
| `menu` | `CATEGORY` | `apps`, `games`, `tools`, `none`; defaults from the category's `menu` in categories.json |
| `stack_kb` | — | Informational: the stack set in the BFLT header |

**Screen rotation.** WhyOS turns the screen with the badge: `sleepd`
reads the accelerometer and sets the panel's rotation and the arrow keys'
mapping (why2025-linux `docs/HOME.md`, "Tilt"). That is what most apps
want, so `rotation = "auto"` is the default. An app or game that uses the
accelerometer itself (tilt controls, a spirit level) sets
`rotation = "locked"`: while it runs, the screen stays upright (rotation
0, the badge's normal orientation), the arrow keys keep their normal
mapping, and `sleepd` stops reading the accelerometer, so the app has it
to itself. When the app ends, automatic rotation resumes. An app that
locks rotation SHOULD also list `accelerometer` in `requires.features`.

`NAME` comes from `name`. The client adds `ID`, `TYPE` and `SUBCATEGORY`
lines to `app.conf`; the current menu ignores unknown keys, a later one
groups by them ([client.md](client.md) §5).

Placeholders in `args` and `files`:

| | Meaning | Today |
|---|---|---|
| `%d` | app directory | exists |
| `%f`, `%u`, `%a`, `%%` | chosen file, file URL, answer to `ask`, percent | exist |
| `%D` | the app's data directory on the card (`storage.data`) | new |
| `%C` | the app's config directory in `$HOME` (`storage.config`) | new |
| `%T` | the app's cache directory (`storage.cache`) | new |

The new placeholders and environment variables are the runtime contract
in [device.md](device.md) §6.

#### 2.3.1 Command-line apps

Many tools are command-line programs (nmap, Links, a Proxmark3 client).
They need no UI of their own: the OS shows them in a generic terminal
window, `appterm` (why2025-linux `docs/APPS.md`, "Command-line apps"). A
package declares this with `launch.terminal`; nothing else in the format
is different.

| `terminal` | Behaviour in the menu | Use for |
|---|---|---|
| `false` (default) | The app draws the panel itself | Games, viewers, browsers with their own UI |
| `true` | Runs in the terminal window; the output and exit status stay up until Enter or Esc | A tool that runs once and reports (`ask` can ask for its arguments first) |
| `"close"` | As `true`, but the window closes when the program ends with status 0 | A tool whose result is not text |
| `"shell"` | The program runs once without arguments, then a shell follows in the window in which its name runs it again, with `args` added | Tools used interactively with changing arguments (nmap) |

What a command-line app gets: a pseudo-terminal of 73 × 25 characters,
`TERM=vt100`, `COLUMNS` and `LINES`, a VT100/ANSI subset with the 16
colours, UTF-8 for box drawing, the keypad as a US keyboard, Ctrl+C as
SIGINT and 300 lines of scrollback. Programs that need more of a
terminal (curses with a full terminfo, mouse input, a wider screen) do
not fit and SHOULD say so in `description`.

**Commands in the shell (optional).** A command-line app can also make
itself available in an ordinary shell (serial console, SSH, or the
`"shell"` window of another app), with a `[[commands]]` entry per
command:

```toml
[[commands]]
name = "nmap"                       # the command; must be unique on the badge
exec = "nmap"                       # program in the app directory
args = "--datadir %d/share --unprivileged --nsock-engine poll"   # put before what the user types
```

The store writes a small wrapper script per command into
`$HOME/.whyos/bin/` (flash; a few hundred bytes each, since the FAT card
cannot hold symlinks and is not always there). The wrapper sets the
`APP_*` variables, expands the placeholders in `args`, and execs the
program with `args` followed by the user's arguments. When the card with
the app is missing, it says so instead of failing with "not found". The
program then runs directly in the user's terminal (serial or SSH, any
size), not in `appterm`.

`$HOME/.whyos/bin` is on the `PATH` in two ways ([client.md](client.md) §5),
so both kinds of shell find the commands:

- interactive and login shells (serial console, `ssh badge`): through
  `/etc/profile.d/whyos-apps.sh`, which `/etc/profile` already reads;
- commands run directly over SSH (`ssh badge nmap 10.0.0.1`), which start
  no login shell and read no profile: through dropbear's `DEFAULT_PATH`,
  with the path written out (`/root/.whyos/bin`), since dropbear does not
  expand `$HOME` there.

The wrapper must be cheap on a NOMMU badge, where a failed fork under
memory pressure breaks a shell (see the comments in the rootfs'
`/etc/profile`):

- it ends with `exec`, so the shell is replaced by the program and no
  extra busybox process stays in the memory pool while it runs;
- it uses only shell builtins and `${…}` parameter expansion: no `$(…)`,
  no backticks, no external commands such as `dirname`.

```sh
#!/bin/sh
# written by store for nmap; do not edit
APP_ID=nmap
APP_DIR=/mnt/sd/badge/apps/nmap
APP_DATA=/mnt/sd/badge/appdata/nmap
APP_CONFIG=/root/.whyos/config/nmap
APP_CACHE=/mnt/sd/badge/appdata/nmap/.cache
WHYOS_API=1
export APP_ID APP_DIR APP_DATA APP_CONFIG APP_CACHE WHYOS_API
if [ ! -x "$APP_DIR/nmap" ]; then
	echo "nmap: not on this SD card; reinstall it in the store" >&2
	exit 127
fi
exec "$APP_DIR/nmap" --datadir "$APP_DIR/share" --unprivileged --nsock-engine poll "$@"
```

The store expands the placeholders when it writes the wrapper, so the
script contains only fixed paths; `args` is split at spaces, as in
`app.conf`. CI rejects a command name that another app
in the index already uses, or that the OS provides (busybox applets and
the rootfs); the client checks again against what is on the badge.
Remove deletes the wrappers ([device.md](device.md) §5.4).

An app may be a command only: `menu = "none"` with `[[commands]]`
leaves it out of the menu's lists and makes it available in the shell.

The store shows command-line apps as such: a "Command-line" label on the
app page and in lists, the commands it adds, and screenshots of the
terminal window (CI can render them from a recorded session, as the
`appterm` tests do).

### 2.4 Requirements

| Field | Req. | Meaning |
|---|---|---|
| `requires.os_api` | yes | Minimum WhyOS API level (integer, from `/etc/whyos-release`, see device.md §6.3) |
| `requires.memory_kb` | yes | Peak use of the user memory pool while running. The client warns when it is above what is free with the menu out of the way |
| `requires.features` | no | Hardware or services the app needs: `wifi`, `bluetooth`, `usb-host`, `sd-card`, `audio`, `camera`, `lora`, `keyboard`, `display`, `accelerometer`, `environment-sensor`. Shown on the info page; the client MAY refuse when a feature is known to be missing |
| `requires.apps` | no | Other app ids that must be installed (e.g. a shared data pack); installed first |

### 2.5 Install location

`install_location`: `sd` (default) or `flash`. Flash (`/data/apps/<id>/`)
is for small apps that must work without a card, such as the store
itself. CI rejects `flash` for packages over 512 KB unpacked; the store
asks before using flash for anything over 128 KB.

### 2.6 Storage

An app writes only to the places it declares. This is what lets the
store remove an app completely, and keep its settings across a card swap.

```toml
[storage.config]          # $HOME/.whyos/config/<id>/, in flash: survives a new card
max_kb = 64               # hard cap, default 16, maximum 256

[storage.data]            # badge/appdata/<id>/ on the card: large data
max_mb = 0                # expected size, 0 = unknown; informational
folders = ["roms", "roms/saves"]     # created at install
user_folders = ["roms"]   # folders the user fills (shown in Files, named on remove)

[storage.cache]           # badge/appdata/<id>/.cache/, may be wiped at any time
enabled = true
```

- **config** is for small settings that should survive a card swap:
  preferences, high scores, a last-opened file. The cap is enforced by
  the store (it reports apps over their cap; it does not stop a running
  app). Flash is shared by all apps and the system, so the sum is kept
  small ([roadmap.md](roadmap.md), open questions).
- **data** is for anything large: game images, save states, downloaded
  content, databases. It is on the card and goes when the card goes.
- **user_folders** are data folders the user puts files into. Remove
  names them and their size before deleting
  ([device.md](device.md) §5.4).
- **cache** can be deleted by the store when space runs out.
- The app directory itself (`%d`) is read-only by convention: it is
  replaced as a whole on update.

### 2.7 Permissions (reserved)

`permissions`: a list from `network`, `usb`, `bluetooth`, `files-all`
(read or write outside its own storage), `system` (changes system
settings). WhyOS cannot enforce these yet (no MMU, everything runs as
root); the store shows them on the info page. The field exists now so
that enforcement later does not need a format change.

### 2.8 Lifecycle hooks (optional)

| Field | When | How |
|---|---|---|
| `hooks.post_install` | after a fresh install | program in the app directory, run once with `APP_*` set |
| `hooks.migrate` | after an update | run with the old and new `build` as arguments |
| `hooks.pre_remove` | before removal | e.g. to tell a service to stop |

Hooks run with a 30 s timeout. A failing `post_install` or `migrate`
marks the app as "needs attention" but does not roll back; a failing
`pre_remove` does not stop the removal. Hooks MUST NOT write outside the
app's own storage.

### 2.9 Generated fields

CI adds these to `manifest.json`; authors do not write them:

| Field | Meaning |
|---|---|
| `published` | ISO 8601 date-time of publication |
| `size.download`, `size.installed` | Bytes of the `.wpk` and of the unpacked `files/` |
| `files.sha256` | Path → SHA-256 for every file under `files/`; the client checks it while unpacking and stores it as `.package/files.sha256` (used by `store verify`) |

The SHA-256 of the whole `.wpk` is in the index, not in the manifest
(the manifest is inside the package).

### 2.10 Licences

Packages are binaries that bundle other people's code, and this
repository is public, so every package carries what its licences ask
for. Three parts:

**1. `license`**: one SPDX expression for everything in the package,
e.g. `GPL-2.0-or-later AND LGPL-2.1-or-later`. The store shows it on the
app page. Licences without an SPDX id use `LicenseRef-<name>`
(`LicenseRef-NPSL-0.95` for nmap).

**2. `[[bundled]]`**: every component that ends up in the package
besides the app's own code: libraries linked in, the C library, data
files, fonts, certificate bundles.

```toml
[[bundled]]
name = "uClibc-ng"
version = "1.0.57"
license = "LGPL-2.1-or-later"
source = "https://downloads.uclibc-ng.org/releases/1.0.57/"
license_file = "uClibc-ng.txt"
```

| Field | Req. | Meaning |
|---|---|---|
| `name`, `version` | yes | As upstream names them |
| `license` | yes | SPDX expression of this component |
| `source` | yes | Upstream source of exactly this version |
| `license_file` | yes | File in `files/LICENSES/` with its licence text and copyright notices |

Compiler runtime libraries covered by the GCC Runtime Library Exception
(libgcc, libstdc++) need not be listed.

**3. `files/LICENSES/`**: the full licence texts, one file per component
(the upstream `COPYING`/`LICENSE` file as is, so copyright notices that
MIT- and BSD-style licences require are kept), plus `<id>.txt` for the
app itself (`license_file` at the top level, default `<id>.txt`). CI
checks that every `license_file` exists and that every SPDX id in
`license` occurs in the app's or a bundled component's licence. The
store's app page has a "Licences" view that lists the components and
opens these texts.

**Source.** Every WhyOS binary links uClibc-ng statically, and the LGPL
then requires that users can relink the app; copyleft apps (GPL) require
the complete corresponding source anyway. So every package with native
code MUST make its corresponding source available: the upstream sources
at pinned versions plus everything that was changed or added for the
badge (patches, new files, build scripts). Either `source` points to a
public place that holds exactly that (a tag or commit, not a branch), or
the app directory in this repository has a `source/` directory with the
badge-specific files and a `SOURCES.md` that lists the pinned upstream
downloads (repository.md §1.2). CI publishes `source/` as
`<id>-<version>-source.tar.gz` next to the package.

Reviewers also check that the licence allows redistribution at all.
Non-free licences (`LicenseRef-…`) are accepted only when they do, and
the app page marks them as "not open source".

## 3. Binaries

The store serves one target: WhyOS on the badge. Programs are static BFLT
binaries for RV32IMAFC without MMU, linked against uClibc-ng with the
badge's toolchain (why2025-linux `sdapps/build.sh`). There is no platform
field and no choice between builds: a package either runs on the badge
or is rejected by CI. Packages without native code (data packs, scripts
for the shell) are fine. When the OS changes in a way apps notice, that is
expressed with `requires.os_api`, not with a platform.
