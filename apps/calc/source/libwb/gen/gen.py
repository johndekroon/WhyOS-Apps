#!/usr/bin/env python3
"""Generate libwb's headers from assets.py: glyph and icon alpha masks.

The badge program has no font or vector renderer (FLAT/NOMMU, see the
Makefile), so everything it draws besides rectangles comes from here,
pre-rendered as alpha:

  * wb_assets.h, 8 bits: Space Grotesk (UI) and JetBrains Mono (data, keys),
    the design system's typefaces, at the sizes and weights the screens
    use, and the icons: the SVG paths of the design's screens, 24x24 viewBox,
    rendered at their on-screen size and stroke width. Both fonts are SIL OFL
    (fonts/). It also tabulates the focus glow's fall-off, since the badge
    has no FPU.
  * wb_doc_fonts.h, 4 bits: the same typefaces with Latin-1, for text people
    type (wbdoc.h).
  * emoji_font.h, 8 bits: the emoji the chat app shows (meshchat, wbdraw.h's
    WB_EMOJI): Noto Emoji (SIL OFL, fonts/), the outline style, so they take
    the text's colour like the rest of the UI. Only the ones people use most,
    as the badge has room for a few hundred, not for all of Unicode's.

Needs python3-pil and python3-cairosvg (in the build VM:
apt install python3-pil python3-cairosvg). The output is checked in, so
building the badge image does not need either.

    python3 gen.py            write the headers
    python3 gen.py --check    compare them with what assets.py gives
"""
import io
import math
import os
import sys

import cairosvg
from PIL import Image, ImageDraw, ImageFont

import assets

HERE = os.path.dirname(os.path.abspath(__file__))


def font_file(name):
    return os.path.join(HERE, "fonts", name)


def c_bytes(data, indent="\t"):
    out = []
    for i in range(0, len(data), 16):
        out.append(indent + ",".join(str(b) for b in data[i:i + 16]) + ",")
    return "\n".join(out)


def gen_font(name, path, size, weight, extra=None):
    font = ImageFont.truetype(font_file(path), size, layout_engine=ImageFont.Layout.BASIC)
    font.set_variation_by_axes([weight])
    ascent, descent = font.getmetrics()
    chars = ([chr(c) for c in range(assets.FIRST, assets.LAST + 1)] +
             list(assets.TERM_EXTRA if extra else assets.EXTRA))
    glyphs, bits = [], bytearray()
    for ch in chars:
        # bbox is relative to the text origin, whose y is the ascender line
        ox, oy, x1, y1 = font.getbbox(ch)
        w, h = max(0, x1 - ox), max(0, y1 - oy)
        data = b""
        if w and h:
            img = Image.new("L", (w, h))
            ImageDraw.Draw(img).text((-ox, -oy), ch, font=font, fill=255)
            data = img.tobytes()
        glyphs.append((len(bits), w, h, ox, oy, round(font.getlength(ch) * 16), ord(ch)))
        bits += data
    lines = ["static const uint8_t %s_bits[] = {\n%s\n};" % (name, c_bytes(bits))]
    lines.append("static const struct wb_glyph %s_glyphs[] = {" % name)
    for off, w, h, ox, oy, adv, cp in glyphs:
        lines.append("\t{ %d, %d, %d, %d, %d, %d, 0x%04x }," % (off, w, h, ox, oy, adv, cp))
    lines.append("};")
    lines.append("static const struct wb_font %s = { %s_glyphs, %s_bits, %d, %d, %d, %d };"
                 % (name, name, name, len(glyphs), ascent, descent, size))
    return "\n".join(lines)


def pack4(img):
    """Two pixels to a byte, high nibble first, each row padded to a whole byte."""
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


def gen_doc_font(name, path, size, weight):
    font = ImageFont.truetype(font_file(path), size, layout_engine=ImageFont.Layout.BASIC)
    font.set_variation_by_axes([weight])
    ascent, descent = font.getmetrics()
    glyphs, bits = [], bytearray()
    for ch in sorted(set(assets.DOC_CHARS), key=ord):
        ox, oy, x1, y1 = font.getbbox(ch)
        w, h = max(0, x1 - ox), max(0, y1 - oy)
        data = b""
        if w and h and ch not in assets.DOC_BLANK:
            img = Image.new("L", (w, h))
            ImageDraw.Draw(img).text((-ox, -oy), ch, font=font, fill=255)
            data = pack4(img)
        else:
            w = h = 0
        glyphs.append((len(bits), w, h, ox, oy, round(font.getlength(ch) * 16), ord(ch)))
        bits += data
    lines = ["static const uint8_t %s_bits[] = {\n%s\n};" % (name, c_bytes(bits))]
    lines.append("static const struct wd_glyph %s_glyphs[] = {" % name)
    for off, w, h, ox, oy, adv, cp in glyphs:
        lines.append("\t{ %d, %d, %d, %d, %d, %d, 0x%04x }," % (off, w, h, ox, oy, adv, cp))
    lines.append("};")
    lines.append("static const struct wd_font %s = { %s_glyphs, %s_bits, %d, %d, %d, %d };"
                 % (name, name, name, len(glyphs), ascent, descent, size))
    return "\n".join(lines)


def gen_icon(name, size, sw, inner):
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 24 24" '
           'fill="none" stroke="#fff" stroke-width="%s" stroke-linecap="round" '
           'stroke-linejoin="round">%s</svg>' % (size, size, sw, inner))
    png = cairosvg.svg2png(bytestring=svg.encode(), output_width=size, output_height=size)
    alpha = Image.open(io.BytesIO(png)).convert("RGBA").getchannel("A")
    return ("static const uint8_t %s_a[] = {\n%s\n};\n"
            "static const struct wb_icon %s = { %d, %d, %s_a };"
            % (name, c_bytes(alpha.tobytes()), name, size, size, name))


# Focus glow (CSS box-shadow 0 0 B px rgba(cyan, opa)): outside the shape the
# blurred edge is opa * erfc(d / (sigma * sqrt 2)) / 2 with sigma = B / 2.
# Indexed by the distance from the edge in quarter pixels.
def gen_glow(name, blur, opa):
    sigma = blur / 2
    vals = []
    for q in range(blur * 2 * 4):
        d = q / 4
        vals.append(round(255 * opa * 0.5 * math.erfc(d / (sigma * math.sqrt(2)))))
    return ("static const uint8_t %s[%d] = {\n%s\n};" % (name, len(vals), c_bytes(bytes(vals))))


def gen_emoji():
    """The emoji in assets.EMOJI, by code point (wbdraw.h finds them by bisection),
    with their own glyph struct: wb_glyph's code point is 16 bits."""
    font = ImageFont.truetype(font_file(assets.EMOJI_FONT), assets.EMOJI_SIZE, layout_engine=ImageFont.Layout.BASIC)
    font.set_variation_by_axes([400])
    ascent, descent = font.getmetrics()
    def render(ch):
        ox, oy, x1, y1 = font.getbbox(ch)
        w, h = max(0, x1 - ox), max(0, y1 - oy)
        img = Image.new("L", (max(w, 1), max(h, 1)))
        ImageDraw.Draw(img).text((-ox, -oy), ch, font=font, fill=255)
        return ox, oy, w, h, img.tobytes()

    notdef = render("\U0010fffd")	# what the font draws for what it lacks
    cps = sorted({c for a, b in assets.EMOJI for c in range(a, b + 1)})
    glyphs, bits, missing = [], bytearray(), []
    for cp in cps:
        ox, oy, w, h, data = render(chr(cp))
        if not (w and h) or (w, h, data) == (notdef[2], notdef[3], notdef[4]):
            missing.append(cp)
            continue
        glyphs.append((cp, len(bits), w, h, ox, oy, round(font.getlength(chr(cp)) * 16)))
        bits += data
    if missing:
        sys.stderr.write("not in the font: %s\n" % " ".join("%X" % c for c in missing))
    out = [
        "/* Generated by libwb/gen/gen.py -- do not edit. */",
        "/* Emoji: Noto Emoji (SIL OFL 1.1, libwb/gen/fonts), %d of them at %d px. */"
        % (len(glyphs), assets.EMOJI_SIZE),
        "#pragma once",
        "#include <stdint.h>",
        "",
        "struct wb_emoji { uint32_t cp, off; uint8_t w, h; int8_t x, y; uint16_t adv16; };",
        "static const uint8_t wb_emoji_bits[] = {\n%s\n};" % c_bytes(bits),
        "static const struct wb_emoji wb_emoji[] = {",
    ]
    for cp, off, w, h, ox, oy, adv in glyphs:
        out.append("\t{ 0x%05x, %d, %d, %d, %d, %d, %d }," % (cp, off, w, h, ox, oy, adv))
    out.append("};")
    out.append("#define WB_EMOJI_N\t%d" % len(glyphs))
    out.append("#define WB_EMOJI_ASCENT\t%d" % ascent)
    out.append("#define WB_EMOJI_SIZE\t%d" % assets.EMOJI_SIZE)
    out.append("")
    return "\n".join(out)


def gen_assets():
    out = [
        "/* Generated by libwb/gen/gen.py -- do not edit. */",
        "/* Fonts: Space Grotesk, JetBrains Mono (SIL OFL 1.1, libwb/gen/fonts). */",
        "#pragma once",
        "#include <stdint.h>",
        "",
        "struct wb_glyph { uint32_t off; uint8_t w, h; int8_t x, y; uint16_t adv16; uint16_t cp; };",
        "struct wb_font { const struct wb_glyph *g; const uint8_t *bits; uint16_t n;",
        "\t\t uint8_t ascent, descent, size; };",
        "struct wb_icon { uint8_t w, h; const uint8_t *a; };",
        "",
    ]
    for f in assets.FONTS:
        out.append(gen_font(*f))
        out.append("")
    for i in assets.ICONS:
        out.append(gen_icon(*i))
        out.append("")
    for g in assets.GLOWS:
        out.append(gen_glow(*g))
        out.append("")
    return "\n".join(out)


def gen_doc():
    out = [
        "/* Generated by libwb/gen/gen.py -- do not edit. */",
        "/* Fonts: Space Grotesk, JetBrains Mono (SIL OFL 1.1, libwb/gen/fonts), 4-bit alpha. */",
        "#pragma once",
        "#include <stdint.h>",
        "",
        "/* glyphs sorted by code point; rows of (w + 1) / 2 bytes */",
        "struct wd_glyph { uint32_t off; uint8_t w, h; int8_t x, y; uint16_t adv16; uint16_t cp; };",
        "struct wd_font { const struct wd_glyph *g; const uint8_t *bits; uint16_t n;",
        "\t\t uint8_t ascent, descent, size; };",
        "",
    ]
    for f in assets.DOC_FONTS:
        out.append(gen_doc_font(*f))
        out.append("")
    return "\n".join(out)


HEADERS = [("wb_assets.h", gen_assets), ("wb_doc_fonts.h", gen_doc), ("emoji_font.h", gen_emoji)]


def main():
    check = sys.argv[1:] == ["--check"]
    if sys.argv[1:] and not check:
        sys.exit(__doc__)
    stale = []
    for name, gen in HEADERS:
        path = os.path.join(HERE, "..", name)
        text = gen()
        if check:
            with open(path, encoding="utf-8") as f:
                if f.read() != text:
                    stale.append(name)
        else:
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
    if stale:
        sys.exit("not what assets.py gives: %s (run gen.py)" % " ".join(stale))


if __name__ == "__main__":
    main()
