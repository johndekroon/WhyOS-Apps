# files: Files, the WhyOS file browser

Files shows every file on the badge as it is: the SD card, USB drives
and the badge's own storage. Until 2026-10-25 it was part of the menu
(why2025-linux `home.c`); it is an app now so that it updates from the
store without a new image. The package is [apps/files](../apps/files): a
system app in flash (`/data/apps/files`), like the store and Write, which
why2025-linux puts in the image. The menu's Files tile starts it; it is
not listed under Apps (`menu = "none"`).

The keys are the menu's Files as they were (WhyBadge
`Docs/06-design-system.md` 6.10, why2025-linux `docs/HOME.md` "Files"):
Drives and the folders; Enter opens a folder, or a file with the app
whose `OPENS` has its extension (one app: at once; more: "Open with");
the square key's options (Open with, Rename, Copy, Cut, Paste here, New
folder, Delete); Del deletes too (the same "Delete …?" with Cancel
first); PgUp and PgDn (Fn + Up, Down) and the arrows across a page of 8;
the triangle `/root`; the cross Drives; Esc up a folder, and closes Files
at `/` and on Drives; the circle the log of copy, move and delete
(`/tmp/files.log`); the cloud "Show hidden files"; the diamond help.

The look (since 2026-10-27) is the canvas "Menu's met frontpaneel" and
its board Files: the frontpanel's figures behind every screen; a folder
under the eyebrow "SD CARD › /home/john · 18 ITEMS · 12 GB FREE", the
title Files and the places as pills (SD card, USB when one is in, System;
the one the folder is on cyan); the folder as one panel of glass with
the columns Name, Size (a folder's entries) and Changed, rows of 48 px,
the focus cyan, rows the panel cuts off faded, a scroll bar in its right
edge; a notice and the clipboard ("Copied: notes.txt", pink) under the
list; the hint bar Enter, PgUp PgDn, Del, the position ("12 / 18") and
Esc. Drives, the settings and the log are cards and panels of glass on
the same scene; options, Open with, Rename, New folder and Delete are
libwb's frosted pop-ups (`menu_popup`, `dialog`). A frame with a pop-up
is drawn whole (1 MB, only while the pop-up is open) so that the pop-up
can blur what is under it; without room for it, in bands with the
pop-up's glass unblurred. The scene turns with the badge: Files looks
four times a second and draws again.

| File | What |
|---|---|
| `src/browse.c` | the folder on screen: reading and sorting it, up and into folders, the clipboard, copy, move, delete, rename and New folder (`cp`, `mv`, `rm` with their output in the log) |
| `src/apps.c` | the installed apps that open files, from their `app.conf` (`OPENS`, `OPENS_ALSO`), as the menu reads them |
| `src/disks.c` | the drives on the Drives screen and mounting them, as the menu's Disks; the SD card's FAT at `/media/sd` while Files is open |
| `src/app.c` | the app as a state machine: keys in; the list, pop-ups and what to open out |
| `src/ui.c` | the screens, with libwb from why2025-linux (`libwb/wb.h`, `wbstatus.h`) |
| `src/main.c` | the badge program (keypad, panel, SIGTERM) and the host program for the screen tests |
| `tests/unit/` | the modules' tests (`t.h` is the harness) |
| `tests/test_ui.py` | the screens on the host: keys from a script, screens as PPM (`SHOTS=dir` keeps them as PNG) |

```
make -C files test        # unit tests, then the screens (needs why2025-linux next to this repo)
files/build.sh            # the badge's build/files (needs ~/br-badge, in the build VM)
```

## Files and the menu

Files does not start apps (apprun and the menu do that). To open a file
it writes the app's folder and the file to `/tmp/files-open`
(`<app dir>\n<file>\n`) and ends; the menu starts that app with
`--browse=<file>` as where to come back, and when the app ends, starts
Files again with the file as its argument: back in the folder, on the
file. When the app did not start, the menu writes why to
`/tmp/files-notice`, which Files shows once. `files ""` opens on Drives;
`files` alone on `/`.

The SD card in the badge layout has its FAT mounted at `/media/sd` only
while Files is open, as before: Files mounts it when it starts and
unmounts it when it closes (not when it opens a file, which may be on
it). The menu unmounts one that was left. One mounted in Settings >
Disks stays.

"Show hidden files" is kept in `$APP_CONFIG/settings` (`hidden=1`); the
first time, Files takes over the menu's old `files.hidden` from
`/etc/badge/settings`.

## Tests on the badge

`/tmp/files.keys` takes key codes from a test (`h108`: a held Down,
`s30`: Shift+A). After every frame drawn while a test drives it,
`/tmp/files.where` says what is on screen: title, folder, focused row,
the right of the title, and the screen (`list`, `settings`, `log`,
`pick`, `input`, `confirm`, `help`). The hardware test is
`Tools/usb/hil/test_hil_files.py` in the WhyBadge workspace.

## Decisions

- A held arrow moves by the keypad's repeats (why2025-linux kernel patch
  0060): one row each, two after 1.5 s, four after 2.5 s, as the menu's
  lists did with their own timer. A repeat that waited more than 100 ms
  is skipped (`keyrep.h`), so the list stops when the key is let go.
- App folders up to 127 bytes (the menu's 63): long SD-card paths and
  the host tests fit.
