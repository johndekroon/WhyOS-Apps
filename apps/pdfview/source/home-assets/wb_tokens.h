/*
 * WhyBadge "Neon Orbit" design tokens.
 * Generated from design/tokens.json. Colours are 0xRRGGBB.
 */
#pragma once

/* Screen */
#define WB_SCREEN_W          720
#define WB_SCREEN_H          720

/* Brand */
#define WB_COLOR_CYAN        0x61F2FF  /* focus, primary */
#define WB_COLOR_PINK        0xF25E95  /* active / on */
#define WB_COLOR_YELLOW      0xFFFB96  /* highlight */

/* Surfaces */
#define WB_COLOR_VOID        0x0A0A14
#define WB_COLOR_CONSOLE     0x06060D
#define WB_COLOR_SURFACE1    0x13131F
#define WB_COLOR_SURFACE2    0x1C1C2C
#define WB_COLOR_LINE        0x2C2C44

/* Text */
#define WB_COLOR_TEXT_HI     0xF2F2FA
#define WB_COLOR_TEXT_MID    0xA6A6C0
#define WB_COLOR_TEXT_LOW    0x7C7C99

/* Status */
#define WB_COLOR_OK          0x6EF2A8
#define WB_COLOR_WARN        0xFFFB96
#define WB_COLOR_ERROR       0xFF7A6E

/* Symbol keys (only for home tiles) */
#define WB_COLOR_KEY_SQUARE   0xFF4D5E
#define WB_COLOR_KEY_TRIANGLE 0xFF9A3D
#define WB_COLOR_KEY_CROSS    0xFFE14D
#define WB_COLOR_KEY_CIRCLE   0x3DDC84
#define WB_COLOR_KEY_CLOUD    0x3D8BFF
#define WB_COLOR_KEY_DIAMOND  0xC45CFF

/* Focus: 2 px cyan border, fill = cyan at 8 %, glow = cyan at 40 % */
#define WB_FOCUS_BORDER_W    2
#define WB_FOCUS_FILL_OPA    20   /* 0-255, ~8 % */
#define WB_FOCUS_GLOW_OPA    102  /* 0-255, ~40 % */
#define WB_FOCUS_GLOW_W      18

/* Type sizes (px) */
#define WB_FONT_DISPLAY      56
#define WB_FONT_TITLE        36
#define WB_FONT_HEADING      28
#define WB_FONT_BODY         24
#define WB_FONT_LABEL        20
#define WB_FONT_CAPTION      18
#define WB_FONT_MONO         20

/* Spacing */
#define WB_SPACE_1           4
#define WB_SPACE_2           8
#define WB_SPACE_3           16
#define WB_SPACE_4           24
#define WB_SPACE_5           32
#define WB_SPACE_6           48
#define WB_SPACE_7           64

/* Radius */
#define WB_RADIUS_SM         8
#define WB_RADIUS_MD         14
#define WB_RADIUS_LG         20

/* Layout */
#define WB_STATUS_BAR_H      48
#define WB_HINT_BAR_H        56
#define WB_CHROME_PAD        24
#define WB_CONTENT_PAD       48
#define WB_LIST_ROW_H        68
#define WB_HOME_TILE_H       164
#define WB_HOME_TILE_GAP     24
