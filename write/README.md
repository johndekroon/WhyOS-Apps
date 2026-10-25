# write: Write, the WhyOS text editor

Write is the text editor of the WhyBadge design (`design/write/` in the
WhyBadge workspace: the screens in `canvas/`, behaviour and keys in
`README.md`). Notes and documents in rich text, saved as markdown; other
text and code in a code view with highlighting. The package is
[apps/write](../apps/write): a system app in flash (`/data/apps/write`),
like the store, listed under Apps; why2025-linux puts it in the image.

| File | What |
|---|---|
| `src/text.c` | the text buffer: lines, insert and delete, undo and redo in steps (typing merges per word, 100 steps), the saved state |
| `src/md.c` | one markdown line as the rich view shows it: the block marker, the inline styles Write knows (`**`, `*`, `***`, `<u>`), the caret map between visible text and source, writing styles back |
| `src/edit.c` | editing in the three modes (rich, markdown source, code): typing, Enter and Backspace rules, styles and paragraph styles, the clipboard, caret moves |
| `src/find.c` | find and replace (§8 of the design) |
| `src/syntax.c` | the twelve languages of the Language screen and their highlighting |
| `src/files.c` | loading (256 KB at most), saving through a temporary file and rename, names, the Open list, dates |
| `src/layout.c` | lines into rows: wrapping, heights (cached per line), caret x/y, up and down |
| `src/app.c` | the app as a state machine: keys in; document, overlays, requests out |
| `src/ui.c` | the screens, with the menu's drawing code from why2025-linux (`wbdraw.h`, `wbstatus.h`, `home_assets.h`) |
| `src/main.c` | the badge program (keypad, panel, SIGTERM, running scripts) and the host program for the screen tests |
| `src/write_fonts.h` | the document fonts and a few icons, from `assets/gen.py` (checked in) |
| `tests/unit/` | the modules' tests (`t.h` is the harness) |
| `tests/test_ui.py` | the screens on the host: keys from a script, screens as PPM |

```
make -C write test        # unit tests, then the screens (needs why2025-linux next to this repo)
write/build.sh            # the badge's build/write (needs ~/br-badge, in the build VM)
python3 write/assets/gen.py > write/src/write_fonts.h   # in the build VM (PIL, cairosvg)
```

The screen tests set `WRITE_UI_CHECK`: every frame drawn band by band
(only the bands whose contents changed) must equal the same frame drawn
whole. Host builds go to `tests/unit/bin-<system>/` and
`write-ui-host-<system>`, as this folder is shared with the build VM.

On the badge: `/tmp/write.keys` takes key codes from a test (`c31` is
Ctrl+S, `f106` Fn+Right); `WRITE_UI_TIMING=1` writes how long each frame
took to stderr (`/tmp/app-write.log`). The clipboard is
`/tmp/write.clip`, the log `/tmp/write.log`, the settings
`$APP_CONFIG/settings`. The hardware test is
`Tools/usb/hil/test_hil_write.py` in the WhyBadge workspace.

## Decisions the design left open

- Italic: Space Grotesk has none; Write slants the regular glyphs.
- Files over 256 KB are refused with a message (README §11's proposal).
- The logo key saves a named document over itself and an unnamed one as
  `/root/untitled.md` (`-2`, `-3` ... when that exists).
- The clipboard is `/tmp/write.clip`: shared between documents, gone after
  a restart; only Write uses it.
- Options has a Language row for every document, so a new document can be
  made a script.
- ▲ in the code view runs Python (MicroPython from the store) and shell
  scripts in appterm's window, and shows HTML in NetSurf; other languages
  have no ▲.
- Help is drawn as the design's Help screen, not with wbdraw's
  `help_popup`: twelve keys do not fit in the shared pop-up.
