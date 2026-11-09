#!/usr/bin/env python3
"""The icons the app repository's format asks for (docs/metadata.md): icon16.png, icon32.png and
icon64.png in apps/<id>/, the app's menu icon (launch.icon) in cyan on the design's dark square.
Drawn from the menu's own icon (WhyOS libwb/gen/assets.py), so the store, the menu and these
agree; an app with an icon.png of its own gets that, scaled.

    tools/make-icons.py --whyos ../WhyOS [<id> ...]

Needs Pillow and CairoSVG (as WhyOS libwb/gen/gen.py does). Writes only what changed.
"""
import argparse
import io
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pkglib  # noqa: E402

# the menu's names for launch.icon (WhyOS home.c icon_named) -> libwb's icon
NAMES = {"terminal": "r_ip", "disk": "r_drive", "games": "r_games"}
CYAN, VOID, EDGE = "#61F2FF", (10, 10, 20, 255), (44, 44, 68, 255)
SIZES = (16, 32, 64)


def svg_of(whyos, icon):
    sys.path.insert(0, os.path.join(whyos, "libwb", "gen"))
    import assets
    want = NAMES.get(icon, "r_" + icon)
    for entry in assets.ICONS:
        if entry[0] == want:
            return entry[-1]
    return dict((e[0], e[-1]) for e in assets.ICONS)["r_count"]   # the menu's own fall-back


def render(svg_body, size):
    import cairosvg
    from PIL import Image, ImageDraw
    scale = 4
    big = size * scale
    im = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    r = big // 5
    d.rounded_rectangle((0, 0, big - 1, big - 1), r, fill=VOID, outline=EDGE, width=max(scale, big // 32))
    pad = big // 6
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="%d" height="%d" fill="none" '
           'stroke="%s" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">%s</svg>'
           % (big - 2 * pad, big - 2 * pad, CYAN, svg_body))
    glyph = Image.open(io.BytesIO(cairosvg.svg2png(bytestring=svg.encode()))).convert("RGBA")
    im.alpha_composite(glyph, (pad, pad))
    return im.resize((size, size), Image.LANCZOS)


def png_bytes(im):
    out = io.BytesIO()
    im.save(out, "PNG", optimize=True)
    return out.getvalue()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--whyos", required=True, help="the WhyOS checkout (for libwb's icons)")
    ap.add_argument("ids", nargs="*")
    a = ap.parse_args()
    apps = os.path.join(pkglib.ROOT, "apps")
    for app_id in a.ids or sorted(os.listdir(apps)):
        d = os.path.join(apps, app_id)
        if not pkglib.manifest_path(d):
            continue
        m = pkglib.load_manifest(d)
        if "withdrawn" in m:
            continue
        own = os.path.join(d, "icon.png")
        for size in SIZES:
            if os.path.isfile(own):
                from PIL import Image
                im = Image.open(own).convert("RGBA").resize((size, size), Image.LANCZOS)
            else:
                im = render(svg_of(a.whyos, m.get("launch", {}).get("icon", "count")), size)
            path = os.path.join(d, "icon%d.png" % size)
            data = png_bytes(im)
            if not os.path.exists(path) or open(path, "rb").read() != data:
                with open(path, "wb") as f:
                    f.write(data)
                print(path)


if __name__ == "__main__":
    main()
