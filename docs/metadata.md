# Manifest format 2: metadata.json

Format 2 writes an app's manifest in the format of the badge.team app repository
([Nicolai-Electronics/app-repository](https://github.com/Nicolai-Electronics/app-repository),
read at commit `f51490f`): the same `metadata.json`, with the same field names, plus one object
`whyos` that holds what a WhyOS package needs and that format has no field for. One file then
describes the app for both: our tools read all of it, and the app repository's tools read their
fields.

Format 1 (`manifest.toml`, [package.md](package.md) §2) stays valid. The badge reads neither
directly: `tools/publish.py` turns both into the same `manifest.json` in the package and the
index, so a store on a badge sees no difference, and an app converted from format 1 to format 2
publishes byte for byte the same package.

## 1. The app repository's format

Every app is a folder named by its slug, with a `metadata.json`, its icons and its files. There
are no versions besides the current one, no checksums and no sizes; a launcher downloads each
file it needs from `<server>/<data_path>/<slug>/<file>`. Their `metadata.json`:

| Field | Type | Required | Rules |
|---|---|---|---|
| `name` | string | yes | 3 to 128 characters of `A-Z a-z 0-9 _ - space . ! @ # $ % ^ & * ( ) / \` |
| `description` | string | yes | Up to 256 characters, the same characters (no comma, colon, apostrophe, quote, `+`, `?`, accents) |
| `repository_url` | string | no | |
| `categories` | list of strings | yes | Free; in use: `tools`, `games`, `emulators`, `multimedia`, `communication`, `hardware`, `system`, `fun`, `demo` |
| `version` | string | yes | Semantic version by convention, not checked |
| `icon` | object | yes | Size to PNG file, at least one: `16x16`, `32x32`, `64x64` |
| `author` | string | yes | |
| `license_type` | string | yes | An SPDX id, or `proprietary` |
| `license_file` | string | no | A file in the folder |
| `application` | list | yes | At least one: what to install on which device |

An `application` entry:

| Field | Type | Required | Meaning |
|---|---|---|---|
| `targets` | list of strings | yes | Device names: `tanmatsu`, `konsool`, `why2025` (the badge's original firmware) |
| `type` | string | yes | `appfs` (an ESP-IDF image), `script` (run by an `interpreter` app), `plugin` |
| `revision` | integer | no | A build counter |
| `bsp_version` | integer | no | The board support package the image needs |
| `executable` | string | no | The file to run |
| `interpreter` | string | no | For `script`: the slug of the app that runs it |
| `assets` | list | no | `{"source_file", "target_file"}`: files copied into the app's folder on the device |
| `external_only`, `external_preferred`, `internal_only`, `internal_preferred` | boolean | no | On the SD card or in internal storage |

No other fields are allowed, at the top or in an entry. A launcher installs the first entry
whose targets name its device.

## 2. A WhyOS app in that format

```json
{
  "name": "Flashlight",
  "description": "The whole screen as a light - white at full or half brightness or red.",
  "repository_url": "https://github.com/johndekroon/WhyOS-Apps",
  "categories": ["tools"],
  "version": "1.1.0",
  "icon": {"16x16": "icon16.png", "32x32": "icon32.png", "64x64": "icon64.png"},
  "author": "WhyOS-Apps contributors",
  "license_type": "MIT AND OFL-1.1 AND LGPL-2.1-or-later",
  "license_file": "flashlight.txt",
  "application": [
    {"targets": ["why2025-whyos"], "type": "whyos", "revision": 5, "executable": "flashlight",
     "external_only": true, "assets": []}
  ],
  "whyos": {
    "format": 2,
    "id": "flashlight",
    "summary": "The whole screen as a light: white at full or half brightness, or red.",
    "description": "Three modes: ...",
    "type": "app",
    "category": "utilities",
    "launch": {"statusbar": "hidden", "info": "a white or red light", "icon": "sun"},
    "requires": {"os_api": 1, "memory_kb": 600, "features": ["display", "keyboard"]},
    "bundled": [...]
  }
}
```

`apps/flashlight/metadata.json` is the whole file.

- **Target and type.** A WhyOS package is a FLAT program for Linux on the badge, which the
  original firmware cannot run, so it has a device name and a type of its own: target
  `why2025-whyos`, type `whyos`. A launcher for the original firmware asks for `why2025` and
  never takes it. An app that exists for both can list both entries in one file.
- **The top level** holds what both formats have: the name, a short description, the version,
  the author, the licence, the icons, where the source is.
- **The WhyOS entry in `application`** holds the build (`revision`), the program
  (`executable`) and where it installs (`external_only` for the card, `internal_only` for
  flash).
- **`whyos`** holds the rest of format 1, with the same names and meaning
  ([package.md](package.md) §2), and `"format": 2`. A field that has a place above is not
  repeated in it.

`description` at the top follows their rules (the characters above, 256 at most) and is what
their tools show; `whyos.summary` is the line the store shows, and `whyos.description` the long
text. `tools/metadata.py` makes their `description` from the summary when converting.

## 3. How the fields map

| Format 1 | Format 2 | Note |
|---|---|---|
| `format = 1` | `whyos.format = 2` | |
| `id` | `whyos.id`, and the folder name | Their slug is the folder name; ours is the id |
| `name` | `name` | Theirs allows 3 to 128 characters of a smaller set; ours up to 32 |
| `version` | `version` | |
| `build` | `application[].revision` | Both an integer counter |
| `summary` | `whyos.summary`; `description` in their characters | |
| `description` | `whyos.description` | They have no long description |
| `author` | `author` | |
| `license` | `license_type` | Ours is an SPDX expression; theirs is meant as one id but not checked |
| `license_file` | `license_file` | Ours in `files/LICENSES/`; theirs in the folder |
| `homepage`, `source` | `repository_url`, `whyos.source` | They have one link |
| `type`, `category` | `whyos.type`, `whyos.category`; `categories` | `categories` is filled from ours (below) |
| `install_location` | `internal_only` or `external_only` | |
| `launch.exec` | `application[].executable` | |
| `launch.*` (the rest), `service`, `commands` | `whyos.launch`, `whyos.service`, `whyos.commands` | |
| `requires`, `storage`, `permissions`, `hooks`, `bundled`, `tags`, `maintainer`, `changelog`, `upstream_version`, `content_warning`, `withdrawn` | `whyos.*` | |

Our categories in theirs: Internet → `communication`; Hardware → `hardware`; Tools and
Other apps → `tools`; Emulators → `games` and `emulators`; other games → `games`; system →
`system`.

## 4. What their format lacks

The fields WhyOS needs that have no place in the app repository's format, and so live in
`whyos`:

| Missing | Why a WhyOS package needs it |
|---|---|
| A permanent id apart from the folder, and tombstones (`withdrawn`) | A removed app's id is never reused, and badges that have it are told |
| A long description, a changelog, tags, a content warning | The store's app page and search |
| A maintainer | Who reviews changes to the app |
| A separate source link per version, and bundled components with their licences (`bundled`) | The licences of the bundled code (LGPL relinking, GPL source) |
| How the app starts: arguments, a terminal, Esc, files it opens, a question before it starts, rotation, the status bar, child programs (`launch`) | The menu writes `app.conf` from it (WhyOS `docs/APPS.md`) |
| Commands in the shell (`commands`), a background service (`service`) | |
| Requirements: OS API level, memory, hardware features, other apps (`requires`) | The store warns or refuses before installing |
| Storage places and their limits (`storage`) | Removing an app completely, settings that survive a card swap |
| Permissions (`permissions`) | The sandbox gives an app only what it lists |
| Hooks after install, on update, before removal (`hooks`) | |
| Checksums and sizes of every file and of the package, the date published | The store checks a download and resumes it |
| Older versions, and a build order that stops downgrades | The store offers an older version and refuses a downgrade |
| Screenshots and a cover picture | The store's app page |

And what theirs has that format 1 lacked, now taken over: the icons at 16, 32 and 64 px
(`tools/make-icons.py` draws them from the app's menu icon, or scales its `icon.png`), several
devices per app (`application`), and the type of executable (`type`, `interpreter`: a Python
app could become a `script` for the MicroPython app).

## 5. The tools

| Command | What |
|---|---|
| `tools/metadata.py convert <id>` | `manifest.toml` to `metadata.json`; refuses when the round trip would change anything, so the package stays the same |
| `tools/metadata.py export <id>` | The app as the app repository takes it: their fields only |
| `tools/metadata.py check <file>` | Their rules for a `metadata.json`, and that the icons and assets it names are in its folder |
| `tools/make-icons.py --whyos <WhyOS> [<id>]` | `icon16.png`, `icon32.png`, `icon64.png` |
| `tools/validate.py` | For a format-2 app also their rules, and that it has one WhyOS entry |

`tools/publish.py` writes `dist/v1/app-repository/` beside the index: every app's folder as the
app repository keeps it (`<id>/metadata.json` with their fields only, and the icons), the folder
their repository server serves; `projects.json`, what that server answers on `GET /projects`,
each project with its `whyos` fields and its package (`url`, `sha256`, `size`); and
`categories.json` and `information.json`. The program itself stays in the WhyOS package: the
folders have no files to download besides the icons.
