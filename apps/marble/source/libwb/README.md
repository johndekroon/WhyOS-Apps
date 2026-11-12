# libwb: WhyOS's drawing library

The menu (`patches/buildroot/overlay-src/home.c`) and the apps draw their
screens with this one library, in the style of the design system "Neon
Orbit". Header-only C, integer maths (the toolchain is soft-float), no
stdio: it builds into FLAT programs.

```c
#include "wb.h"          /* with -I libwb */
```

`W` and `H` default to the panel (720 x 720); a program may define them
first. Everything is `static`; `--gc-sections` drops what a program does not
use. A program draws into `fb` (RGB565), rows `fb_y0`..`fb_y1`: the whole
panel from a 1 MB shadow buffer, or one band at a time (`fb` then points at
row 0, outside the band), and writes it to `/dev/fb0` itself.

## The parts

| Header | What |
|---|---|
| `wb.h` | include this; sets the style (square corners, pop-ups) and includes the rest |
| `wb_tokens.h` | colours, sizes, spacing |
| `wb_assets.h` | fonts, icons and the glow fall-off (`gen/assets.py`) |
| `wbdraw.h` | pixels (`put`, `fill`, `dim`), boxes (`box`), text (`text`, `text_w`, `text_top`, `wb_fit`, `text_wrap`), icons, key caps, `hint_bar`, `app_bar`, `eyebrow`, `help_popup`, `scrollbar`, `list_scroll`, `clip_rows`, `tick`, `empty_page`, `empty_screen` |
| `wbscene.h` | the frontpanel's figures behind the screen (moon with craters, orbits, head), turned with the badge and coloured; `scene_draw`, `rot`, `rot_read` |
| `wbglass.h` | `glass()` boxes and frosted pop-ups (`glass_backdrop`), `GLOW_FOCUS`, `box_shaped`, `dim_outside` |
| `wbui.h` | `wb_scene`, `wb_scene_turned`, `wb_bands`, `wb_frame`, `wb_out_wait`, `wb_header`, `wb_row`, `wb_switch`, `step_button`, `edge_rect`, `icon_scaled`, `loupe`, `menu_popup`, `dialog`, `dlg_hints`, `panel_hints` |
| `wbstatus.h` | the status bar's clock, sound, Wi-Fi and LoRa (`status_right`), for sysui, which draws the bar over every app; apps do not include it |
| `wbdoc.h` | text people type, in UTF-8 with Latin-1: the document fonts (`wd_ui24`, `wd_mono20` and so on), `wd_text`, `wd_text_w`, `wd_top`, `wd_fit`, `wd_glyph_draw` |
| `keyrep.h` | which keys repeat when held: the arrows, Page Up/Down, Backspace, Delete, Space and the typing keys |
| `lora_state.h` | whether the LoRa radio is on |
| `sound_state.h` | where sound plays and how loud, as sysui says it (docs/SOUND.md), for the status bar and Settings |
| `wb_scene_data.h`, `wb_craters.h`, `wb_doc_fonts.h`, `emoji_font.h` | data (`gen/scene.py`, `gen/craters.py`, `gen/assets.py`) |

## A screen in the design's style

1. `wb_scene()`: the figures as the background. It also marks the frame as
   "on the scene": from then on `wb_card`, `wb_panel` and `hint_bar` draw
   glass. It returns 1 when the badge turned since the last frame: draw
   the whole screen then. While waiting for a key, poll
   `wb_scene_turned()` (four times a second is plenty) and draw again.
   A screen whose content fills it (a picture, a game) skips the scene.
2. The top `WB_STATUS_BAR_H` (48) rows are the status bar's, which the
   system draws over every app (docs/HOME.md): draw only the background
   there. Then `wb_header(eyebrow, title, right)`:
   the eyebrow says where the screen lies ("APP", "SETTINGS ›",
   "SD CARD › /home"), the title 36 px; rows start at `WB_ROWS_TOP` (160).
3. Content on cards: `wb_card(x, y, w, h, r, focused, &GLOW_FOCUS)` is
   one flat colour, the void with the design's faint sheen averaged in,
   and a light edge; the focus is a flat cyan tint with a 2 px border and
   glow. (The design has the void at 85 % over the scene with a sheen
   from 7 % to 2 %: on the panel that is 2-4 levels of RGB565, which
   shows as stripes, or dithered as grain behind the text.) `wb_row()` is
   the design's list row (icon, label, optional second line, a value in mono, a chevron or
   the switch). Corners are square (radius 4) whatever radius is asked.
4. `app_bar(hints, n)`: an app screen's keys at the bottom: the screen's
   own keys on the left (Enter first, then ■ ▲ ✕, then other keys), ● ♣ ◆
   small with their words (log, settings, help), and Esc (the hint with
   `right` set) on the far right. What does not fit drops out from the end.
   `hint_bar(hints, n)` draws keys without ● ♣ ◆: a pop-up's or dialog's,
   and the menu's. `empty_screen()` is the log or settings screen of an app
   that has none ("No log yet.", "This app has no settings.").
5. Pop-ups are frosted (the void at 60 % over what is under them, blurred
   18 px), 96 px from the sides, with their keys inside at the bottom:
   `menu_popup()` for a context menu (`cur` -2) or a choice (`cur` the
   present one, -1 when it is filtered out), `dialog()` + `dlg_hints()`
   for a question; `help_popup()` for the diamond key's help (its Esc
   inside too). Blurring needs the whole picture in `fb`: a program
   drawing in bands gets unblurred glass there, with the void at 95 %
   instead of 60 % so what is under it does not read through. A program
   whose `fb` holds the whole panel while it redraws one band (the menu)
   sets `fb_whole = 1`; otherwise only a frame drawn whole
   (`fb_y0 == 0 && fb_y1 == H`) is frosted. The apps that want the blur
   draw the frame whole into a 1 MB buffer while a pop-up is open.

## Drawing only what changed

A frame on the scene costs ~300 ms (the scene ~60, glass ~90,
writing it out ~65, the rest text and icons), so a program should not draw
the whole screen for a key that moves the focus. libwb records a frame
first: with `wb_rec` set every drawing call draws nothing but hashes its
arguments into the strips of 8 rows it touches; the bands whose strips
changed since the frame before are then drawn and written out.

- `wb_bands(paint, arg, band_buf, band_h, out)`: a program drawing in bands
  (fb a band of `band_h` rows in `band_buf`). `paint(arg)` draws the whole
  screen as usual; `out(band, y0, y1)` writes a band to the panel. Returns
  the bands drawn. A focus move draws its two rows' bands (~145 ms instead
  of ~300 in Files).
- `wb_frame(paint, arg, buf, band_h, out)`: the same for a frame in a whole
  buffer (a program takes 1 MB while a pop-up is open, so it is frosted).
  The pop-up's blur is kept while what is under it stays the same, so a
  key that moves the pop-up's focus draws the pop-up's changed bands
  (~200 ms instead of ~500).
- Both wait for the band to be copied before its buffer takes other rows:
  in the sandbox a write of 64 KB or more returns before that. A program
  that writes that much at once and reuses the buffer calls `wb_out_wait()`
  after the write.
- `paint()` runs once more than the bands drawn: it must only draw (no
  reading keys, no counters). It must call `wb_scene()` itself (the frame's
  rotation is read while recording and kept while drawing).
- Drawing that writes `fb` itself (not through libwb) must, while
  `wb_rec` is set, call `wb_rec_rows(y0, y1, key, size)` with what decides
  its pixels instead, and not touch `fb`.
- `wb_rec_invalidate()`: the next frame draws every band (after the program
  drew something else on the panel: a message, a frame drawn its own way).
  `WB_FULL=1` in the environment draws every band always: host tests run
  the same keys with and without it and compare the screens.

Colours: text `WB_COLOR_TEXT_HI`, secondary `WB_COLOR_TEXT_MID` (#C8C8DC),
a field's hint `WB_COLOR_PLACEHOLDER`, focus `WB_COLOR_CYAN`, active/on
`WB_COLOR_PINK`. Keys (the symbol keys, Esc, the hint bar's order) follow
"Writing an app" in [`../docs/APPS.md`](../docs/APPS.md).

## Cost

- The scene is kept per rotation as runs (about 80 KB) the first time it
  is drawn; a whole frame of it then costs ~55-65 ms on the badge. Drawing
  in bands, the first frame of a rotation builds it 40 rows at a time
  (`scene_keep_banded`) with a 57 KB buffer of its own.
- Working a rotation's scene out takes about 0.4 s, once per program. A
  program that starts often can keep the runs in files:
  `wb_scene_cache(dir, key)` before the first frame, with a `key` that
  changes with the program's build, and `wb_scene_cache_flush()` while it
  waits for a key (it writes one rotation and returns 1, or 0 when there is
  nothing to write). The first whole frame then reads the file. A file of
  another build, of other figures, cut short or with a run that does not
  fit its row is not used. The menu does this, on the SD card.
- Glass costs a blend per pixel over what is drawn; a frosted pop-up adds
  the blur of the box under it (~40 ms for a large one) and, the first
  time, a 97 KB buffer for it (a program that never frosts never takes
  it).
- Fonts and icons are pre-rendered alpha masks (`wb_assets.h`, ~1 MB of
  source; a program links only those it uses).

## Changing it

- Fonts, icons, glows, emoji: all of them are listed in `gen/assets.py`,
  also those only one app uses: a program links only what it uses. After a
  change, `gen/gen.py` (needs python3-pil, python3-cairosvg) writes
  `wb_assets.h`, `wb_doc_fonts.h` and `emoji_font.h`;
  `make -C libwb/test assets` checks that they are up to date.
- The figures: `gen/scene.py` from the frontpanel's KiCad file; the craters:
  `gen/craters.py` turns the mask `gen/moon-craters.png` into
  `wb_craters.h`, and `gen/craters.py --from <picture>` makes that mask
  from a 720 x 720 picture of the scene.
- Tokens: `wb_tokens.h`.
- `test/`: `make -C libwb/test` checks on the host that the scene drawn in
  bands is the scene drawn whole (every rotation), that a screen recorded
  and drawn only where it changed (`wb_bands`, `wb_frame`) equals it drawn
  whole after every change, and the menu's preview
  (`patches/buildroot/overlay-src/home-assets/preview.sh`) renders every
  screen of the menu.
- Apps published in WhyOS-Apps carry a copy of the library in their source
  (`apps/<id>/source/libwb/`), as built.
