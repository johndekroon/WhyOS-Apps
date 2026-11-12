# write: Write, the WhyOS text editor

Write is the text editor of WhyOS. Notes and documents in rich text, saved
as markdown; other text and code in a code view with highlighting. The
package is [apps/write](../apps/write): a system app in flash
(`/data/apps/write`), like the store, listed under Apps; WhyOS puts it in
the image.

| File | What |
|---|---|
| `src/text.c` | the text buffer: lines, insert and delete, undo and redo in steps (typing merges per word, 100 steps), the saved state |
| `src/md.c` | one markdown line as the rich view shows it: the block marker, the inline styles Write knows (`**`, `*`, `***`, `<u>`), the caret map between visible text and source, writing styles back |
| `src/edit.c` | editing in the three modes (rich, markdown source, code): typing, Enter and Backspace rules, styles and paragraph styles, the clipboard, caret moves |
| `src/find.c` | find and replace |
| `src/tags.c` | the HTML element still open before the caret, so `</` in the code view finishes its closing tag |
| `src/syntax.c` | the twelve languages of the Language screen and their highlighting |
| `src/files.c` | loading (256 KB at most), saving through a temporary file and rename, names, the Open list, dates |
| `src/layout.c` | lines into rows: wrapping, heights (cached per line), caret x/y, up and down |
| `src/app.c` | the app as a state machine: keys in; document, overlays, requests out |
| `src/ui.c` | the screens, drawn with libwb from WhyOS (`libwb/wb.h`, `wbdoc.h` for the document) in the menu's style |
| `src/repeat.c` | a key held down repeats, by Write's own timer: which keys, when |
| `src/main.c` | the badge program (keypad, panel, SIGTERM, SIGTSTP and SIGCONT, running scripts) and the host program for the screen tests |
| `tests/unit/` | the modules' tests (`t.h` is the harness) |
| `tests/test_ui.py` | the screens on the host: keys from a script, screens as PPM |

```
make -C write test        # unit tests, then the screens (needs WhyOS: set WHYOS to your checkout)
write/build.sh            # the badge's build/write (also needs a Buildroot build of WhyOS: set BR_OUT)
```

Fonts and icons live in WhyOS: add them to `libwb/gen/assets.py`.

The screen tests set `WRITE_UI_CHECK`: every frame drawn band by band
(only the bands whose contents changed) must equal the same frame drawn
whole. Host builds go to `tests/unit/bin-<system>/` and
`write-ui-host-<system>`, so a folder shared with a build VM keeps both.

The screens as PNG: `SHOTS=dir python3 -m pytest -q tests` on Linux (needs
PIL); on macOS the screen tests stop at ASan's `detect_leaks`, which the
platform lacks (`ASAN_OPTIONS` in `tests/test_ui.py`).

On the badge: `/tmp/write.keys` takes key codes from a test (`c31` is
Ctrl+S, `f106` Fn+Right); `WRITE_UI_TIMING=1` writes how long each frame
took to stderr (`/tmp/app-write.log`). The clipboard is
`/tmp/write.clip`, the log `/tmp/write.log`, the settings
`$APP_CONFIG/settings`.

## Decisions

- Italic: Space Grotesk has none; Write slants the regular glyphs.
- Files over 256 KB are refused with a message.
- When the system ends Write (SIGTERM) or pauses it (SIGTSTP), Write saves
  a named document over itself and an unnamed one as `/root/untitled.md`
  (`-2`, `-3` ... when that exists). At a pause the unnamed document becomes
  that file, so the next pause saves over it and makes no new one; the
  caret, the selection and an open pop-up stay as they are, and Write stops
  itself (SIGSTOP). On SIGCONT it draws the whole screen again.
- The clipboard is `/tmp/write.clip`: shared between documents, gone after
  a restart; only Write uses it.
- Options has a Language row for every document, so a new document can be
  made a script.
- ▲ in the code view runs Python (MicroPython from the store) and shell
  scripts in appterm's window, and shows HTML in NetSurf; other languages
  have no ▲.
- As on a phone, a letter typed at the start of a paragraph or after
  `. `, `! ` or `? ` in the rich view becomes a capital; Settings switches
  it off ("Capital at sentence start", on by default). Not in code blocks,
  the markdown view or code.
- In HTML and PHP, `/` typed after `<` finishes the closing tag of the
  innermost open element; on a line with only blanks before it, the line
  takes the indent of the opening tag.
- A held key repeats after 400 ms, every 50 ms: the arrows and Backspace
  (also with Ctrl, Alt or Fn), Space and the keys that type, not Enter,
  Tab, Esc or the symbol keys. Write's own timer, as the menu's lists:
  the keypad's own repeats (WhyOS kernel patch 0060, the same
  pace) are ignored, so a repeat comes only after the last one is drawn.
- Help is Write's own screen, not libwb's `help_popup`: twelve keys, some with three caps, do not fit in the
  shared pop-up. Open and the log have help of their own.
- The look: the frontpanel's scene at the top, where WhyOS draws its
  status bar over the app (Write leaves those 48 px to the system and
  draws no clock or status icons of its own), the document
  bar (its chips glass) and the key bar (glass); the text on an opaque
  page between them. Glass under 24 px text would
  show the scene's lines through the letters, and the page's bands are the
  ones typing and scrolling redraw: there the scene costs nothing, it is
  drawn only in the bands of the bars. Full screen has no scene (the text
  fills the panel). Open and the log are screens on the scene with libwb's
  header ("WRITE ›" and the folder as the eyebrow) and glass cards.
- The key bar of the editor, Open and the log is libwb's `app_bar`: Enter
  first, then the square, triangle and cross, then other keys (Shift+Enter,
  Ctrl+C); the circle, clover and diamond small with their words, and Esc
  on the right. What does not fit drops from the end; Help lists every
  key. While Fn is held a pink Fn follows the keys, where there is room.
- The circle, clover and diamond work on the editor, Open and the log: the
  log is a screen, the settings and the help are pop-ups. The clover on
  the log opens the settings in its place; the help over the log goes
  back to it.
- Pop-ups are glass panels 96 px from the sides with their keys inside at
  the bottom; the key bar under them stays the screen's, dimmed (in full
  screen there is none). Language is libwb's `menu_popup`, Discard,
  Replace? and errors its `dialog`; Options, Settings, the find menu,
  Style, Save as and Help are Write's own in the same style. Drawn in
  bands, the panels are not frosted: libwb draws them as unblurred glass
  with the void at 95 %, so the text under it does not show through.
- The badge turning: the main loop wakes at least every 250 ms and each
  frame reads the rotation; a turn redraws the screen (the scene turns).
  Switches are libwb's (cyan when on).
