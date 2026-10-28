# libwb: WhyOS's drawing library

The menu (`patches/buildroot/overlay-src/home.c`) and the apps draw their
screens with this one library, in the style of the design system "Neon
Orbit" and the canvas "Menu's met frontpaneel" (claude.ai, private;
Docs/06-design-system.md). Header-only C, integer maths (the toolchain is
soft-float), no stdio: it builds into FLAT programs.

```c
#include "wb.h"          /* with -I why2025-linux/libwb */
#include "wbstatus.h"    /* when the screen has the status bar */
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
| `wb_tokens.h` | colours, sizes, spacing (from `design/tokens.json`) |
| `wb_assets.h` | fonts, icons and the glow fall-off (`gen/gen.py`) |
| `wbdraw.h` | pixels (`put`, `fill`, `dim`), boxes (`box`), text (`text`, `text_w`, `text_top`, `wb_fit`, `text_wrap`), icons, key caps, `hint_bar`, `eyebrow`, `help_popup`, `scrollbar`, `list_scroll`, `clip_rows`, `tick`, `empty_screen` |
| `wbscene.h` | the frontpanel's figures behind the screen (moon with craters, orbits, head), turned with the badge and coloured; `scene_draw`, `rot`, `rot_read` |
| `wbglass.h` | `glass()` boxes and frosted pop-ups (`glass_backdrop`), `GLOW_FOCUS`, `box_shaped`, `dim_outside` |
| `wbui.h` | `wb_scene`, `wb_scene_turned`, `wb_bands`, `wb_frame`, `wb_header`, `wb_row`, `wb_switch`, `step_button`, `edge_rect`, `icon_scaled`, `loupe`, `menu_popup`, `dialog`, `dlg_hints`, `panel_hints` |
| `wbstatus.h` | the status bar's clock, Wi-Fi and LoRa (`status_right`) |
| `keyrep.h` | which keys repeat when held (Docs/06 6.6) |
| `wb_scene_data.h`, `wb_craters.h`, `emoji_font.h` | data (`gen/scene.py`, `gen/craters.py`, `gen/gen.py --emoji`) |

## A screen in the canvas's style

1. `wb_scene()`: the figures as the background. It also marks the frame as
   "on the scene": from then on `wb_card`, `wb_panel` and `hint_bar` draw
   glass. It returns 1 when the badge turned since the last frame: draw
   the whole screen then. While waiting for a key, poll
   `wb_scene_turned()` (four times a second is plenty) and draw again.
   A screen whose content fills it (a picture, a game) skips the scene.
2. The status bar (`wbstatus.h`), then `wb_header(eyebrow, title, right)`:
   the eyebrow says where the screen lies ("APP", "SETTINGS ›",
   "SD CARD › /home"), the title 36 px; rows start at `WB_ROWS_TOP` (160).
3. Content on cards: `wb_card(x, y, w, h, r, focused, &GLOW_FOCUS)` is
   one flat colour, the void with the canvas's faint sheen averaged in,
   and a light edge; the focus is a flat cyan tint with a 2 px border and
   glow. (The canvas has the void at 85 % over the scene with a sheen
   from 7 % to 2 %: on the panel that is 2-4 levels of RGB565, which
   showed as stripes, or dithered as grain behind the text.) `wb_row()` is the canvas's list
   row (icon, label, optional second line, a value in mono, a chevron or
   the switch). Corners are square (radius 4) whatever radius is asked.
4. `hint_bar(hints, n)`: the keys at the bottom, Esc on the right.
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

A frame on the scene costs ~300 ms on the badge (the scene ~60, glass ~90,
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

Colours: text `WB_COLOR_TEXT_HI`, secondary `WB_COLOR_TEXT_MID` (#C8C8DC
since 2026-10-27), a field's hint `WB_COLOR_PLACEHOLDER`, focus
`WB_COLOR_CYAN`, active/on `WB_COLOR_PINK`. Keys follow Docs/06 6.6–6.8
(the symbol keys, Esc, the hint bar's order).

## Cost

- The scene is kept per rotation as runs (about 80 KB) the first time it
  is drawn; a whole frame of it then costs ~55-65 ms on the badge. Drawing
  in bands, the first frame of a rotation builds it 40 rows at a time
  (`scene_keep_banded`) with a 57 KB buffer of its own.
- Glass costs a blend per pixel over what is drawn; a frosted pop-up adds
  the blur of the box under it (~40 ms for a large one) and, the first
  time, a 97 KB buffer for it (a program that never frosts never takes
  it).
- Fonts and icons are pre-rendered alpha masks (`wb_assets.h`, ~1 MB of
  source; a program links only those it uses).

## Changing it

- Fonts, icons, glows: `gen/gen.py` (needs python3-pil, python3-cairosvg;
  in the build VM) writes `wb_assets.h`.
- The figures: `gen/scene.py` from the frontpanel's KiCad file; the craters:
  `gen/craters.py --from <the canvas's scene picture>`.
- Tokens: `design/tokens.json`, `design/tokens.h` and `wb_tokens.h` agree.
- `test/`: `make -C libwb/test` checks on the host that the scene drawn in
  bands is the scene drawn whole (every rotation), that a screen recorded
  and drawn only where it changed (`wb_bands`, `wb_frame`) equals it drawn
  whole after every change, and the menu's preview
  (`patches/buildroot/overlay-src/home-assets/preview.sh`) renders every
  screen of the menu.
- Apps published in WhyOS-Apps carry a copy of the library in their source
  (`apps/<id>/source/libwb/`), as built.
