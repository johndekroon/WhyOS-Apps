#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate src/write_fonts.h: the fonts Write draws documents in.

The menu's fonts (why2025-linux home_assets.h) have ASCII and a few symbols:
enough for the menu, not for a text editor, where é, ü, € and curly quotes
must show. These are the same typefaces, Space Grotesk and JetBrains Mono
(SIL OFL, from why2025-linux home-assets/fonts), at the sizes of the design
(design/write: body 24, headings 36/28/24, mono 20/18, and the small text
size's 20/28/24/20, mono 18/16), with ASCII, Latin-1 and the punctuation
people type. Alpha is 4 bits per pixel, two pixels to a byte, high nibble
first, each glyph row padded to a whole byte: half the flash of 8 bits, and
the difference does not show at these sizes.

Italic is not here: Space Grotesk has none, and Write slants the regular
glyphs when it draws them (design/write/README.md §11).

Also the icons of the design that the menu's assets lack, from their SVG
paths (24x24 viewBox), as 8-bit alpha like the menu's.

Needs python3-pil and python3-cairosvg (in the build VM). The output is
checked in.

    python3 assets/gen.py > src/write_fonts.h
"""
import io
import os
import sys

import cairosvg
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
WHY = os.environ.get("WHY2025_LINUX", os.path.join(HERE, "..", "..", "..", "why2025-linux"))
FONTS = os.path.join(WHY, "patches", "buildroot", "overlay-src", "home-assets", "fonts")
UI = os.path.join(FONTS, "SpaceGrotesk-VF.ttf")
MONO = os.path.join(FONTS, "JetBrainsMono-VF.ttf")

CHARS = ([chr(c) for c in range(0x20, 0x7f)] + [chr(c) for c in range(0xa1, 0x100)] +
         list("–—‘’‚“”„•…€™"
              "←↑→↓− "))

SETS = [
    ("wf_ui24", UI, 24, 400), ("wf_ui24b", UI, 24, 700), ("wf_ui36b", UI, 36, 700),
    ("wf_ui28b", UI, 28, 700), ("wf_ui20", UI, 20, 400), ("wf_ui20b", UI, 20, 700),
    ("wf_mono20", MONO, 20, 400), ("wf_mono18", MONO, 18, 400), ("wf_mono16", MONO, 16, 400),
    ("wf_mono14", MONO, 14, 400),
]


# name, size, stroke width, SVG (the design's paths)
ICONS = [
    ("wi_plus", 22, 1.9, '<path d="M12 5v14M5 12h14"/>'),
    ("wi_folder", 22, 1.9, '<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>'),
    ("wi_doc", 22, 1.9, '<path d="M6 3h8l4 4v14H6zM14 3v4h4M9 12h6M9 16h6"/>'),
    ("wi_code", 22, 1.9, '<path d="M8 7l-5 5 5 5M16 7l5 5-5 5"/>'),
    ("wi_code14", 14, 2.2, '<path d="M8 7l-5 5 5 5M16 7l5 5-5 5"/>'),
    ("wi_chev", 18, 2, '<path d="M9 6l6 6-6 6"/>'),
    ("wi_check", 20, 2.4, '<path d="M5 12.5l4.5 4.5L19 7.5"/>'),
    ("wi_trash", 20, 2, '<path d="M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13"/>'),
    ("wi_copy", 12, 2.2, '<path d="M9 9h11v11H9zM5 15H4V4h11v1"/>'),
    ("wi_triangle", 12, 2.8, '<path d="M12 4l9 16H3z"/>'),
    ("wi_drive", 22, 1.9, '<rect x="3" y="6" width="18" height="12" rx="2"/><path d="M7 14h.01M11 14h6"/>'),
]


def gen_icon(name, size, sw, inner):
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 24 24" '
           'fill="none" stroke="#fff" stroke-width="%s" stroke-linecap="round" '
           'stroke-linejoin="round">%s</svg>' % (size, size, sw, inner))
    png = cairosvg.svg2png(bytestring=svg.encode())
    img = Image.open(io.BytesIO(png)).convert("RGBA")
    a = img.split()[3].tobytes()
    return ("static const uint8_t %s_a[] = {\n%s\n};\n"
            "static const struct wf_icon %s = { %d, %d, %s_a };" % (name, c_bytes(a), name, size, size, name))


def c_bytes(data, indent="\t"):
    return "\n".join(indent + ",".join(str(b) for b in data[i:i + 20]) + ","
                     for i in range(0, len(data), 20))


def pack4(img):
    w, h = img.size
    px = img.tobytes()
    out = bytearray()
    for y in range(h):
        row = px[y * w:(y + 1) * w]
        for x in range(0, w, 2):
            a = (row[x] * 15 + 127) // 255
            b = (row[x + 1] * 15 + 127) // 255 if x + 1 < w else 0
            out.append(a << 4 | b)
    return bytes(out)


def gen(name, path, size, weight):
    font = ImageFont.truetype(path, size, layout_engine=ImageFont.Layout.BASIC)
    font.set_variation_by_axes([weight])
    ascent, descent = font.getmetrics()
    glyphs, bits = [], bytearray()
    for ch in sorted(set(CHARS), key=ord):
        ox, oy, x1, y1 = font.getbbox(ch)
        w, h = max(0, x1 - ox), max(0, y1 - oy)
        data = b""
        if w and h and ch not in "  ":
            img = Image.new("L", (w, h))
            ImageDraw.Draw(img).text((-ox, -oy), ch, font=font, fill=255)
            data = pack4(img)
        else:
            w = h = 0
        glyphs.append((len(bits), w, h, ox, oy, round(font.getlength(ch) * 16), ord(ch)))
        bits += data
    out = ["static const uint8_t %s_bits[] = {\n%s\n};" % (name, c_bytes(bits))]
    out.append("static const struct wf_glyph %s_glyphs[] = {" % name)
    for off, w, h, ox, oy, adv, cp in glyphs:
        out.append("\t{ %d, %d, %d, %d, %d, %d, 0x%04x }," % (off, w, h, ox, oy, adv, cp))
    out.append("};")
    out.append("static const struct wf_font %s = { %s_glyphs, %s_bits, %d, %d, %d, %d };"
               % (name, name, name, len(glyphs), ascent, descent, size))
    return "\n".join(out), len(bits)


def main():
    out = [
        "/* Generated by write/assets/gen.py -- do not edit. */",
        "/* Fonts: Space Grotesk, JetBrains Mono (SIL OFL 1.1), 4-bit alpha. */",
        "#pragma once",
        "#include <stdint.h>",
        "",
        "/* glyphs sorted by code point; rows of (w + 1) / 2 bytes */",
        "struct wf_glyph { uint32_t off; uint8_t w, h; int8_t x, y; uint16_t adv16; uint16_t cp; };",
        "struct wf_font { const struct wf_glyph *g; const uint8_t *bits; uint16_t n;",
        "\t\t uint8_t ascent, descent, size; };",
        "struct wf_icon { uint8_t w, h; const uint8_t *a; };",
        "",
    ]
    total = 0
    for s in SETS:
        text, n = gen(*s)
        out.append(text)
        out.append("")
        total += n
    for i in ICONS:
        out.append(gen_icon(*i))
        out.append("")
    out.append("/* %d bytes of glyphs */" % total)
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
