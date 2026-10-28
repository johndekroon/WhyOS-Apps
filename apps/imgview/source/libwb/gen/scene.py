#!/usr/bin/env python3
"""Generate wb_scene_data.h: the frontpanel's figures, continued on the screen.

The WHY2025 frontpanel (Hardware/Frontpanel/frontpanel.kicad_pcb) draws the
screen as a window in front of a scene: a moon with three orbits top left,
and a wireframe head bottom right whose skull disappears behind the screen.
The home screen draws what the window hides, so the panel's lines run on
across the screen:

  * the moon: a disc of the panel's radius in a dim tone, its edge in the
    tone of the lines;
  * the orbits: the panel's three circles around the moon;
  * the head: the outline of the skull from the forehead up over the crown
    to the right edge; rings, which are the outline scaled about a point
    just past the screen's bottom right corner; and meridians, which fan
    up from the brow and leave through the right edge or the outline.
    Every ring and meridian starts where a line of the panel's mesh meets
    the cut-out.

Measured in board millimetres from frontpanel.kicad_pcb on 2026-10-26
(--measure prints the numbers again). The active area is 70.94 mm wide
(the 3.95-inch diagonal) and centred in the cut-out of footprint
W395HDC001-A at (75, 70): 720 px over 70.94 mm, 10.15 px/mm.

Output: circles and polylines in panel pixels (rotation 0), in 1/16 px.
home.c ("Home") draws them anti-aliased, turned with the panel so they stay
in line with the frontpanel when sleepd turns the picture.

    python3 scene.py > ../wb_scene_data.h
    python3 scene.py --measure frontpanel.kicad_pcb
    python3 scene.py --preview out.png [--pcb frontpanel.kicad_pcb] [--screen home.png]

--preview draws the scene in white, or --screen's picture (a screen from
preview.sh, turned as the panel shows it); with --pcb it puts that in the
frontpanel, to check that the lines meet. Needs python3-numpy and
python3-pil. The output is checked in.
"""
import argparse
import math
import re
import sys

import numpy as np
from PIL import Image, ImageDraw

W = H = 720
CUT = (39.035, 34.035, 110.965, 105.965)  # the frontpanel's cut-out (mm)
AA = 70.94                                 # the active area (mm)
AA0 = (75 - AA / 2, 70 - AA / 2)
PX = W / AA                                # px per mm
BOT, RGT = CUT[3], CUT[2]

MOON_C = (53.20, 39.55)
MOON_R = 21.45
MOON_EDGE = 0.30                           # line width (mm) of the moon's edge
# radius, line width (mm); the widths are the runs where the lines cross the cut-out
ORBITS = [(23.24, 0.17), (27.74, 0.20), (36.82, 0.15)]
LINE_W = 0.18                              # the head's mesh
OUTLINE_W = 0.30                           # and its outline

# Where the panel's mesh meets the cut-out (--measure): x on the bottom
# edge, y on the right edge. The first of each is the outline; it leaves
# the bottom edge leaning OUTLINE_LEAN mm per mm (going down, toward the
# back of the head). The steep lines on the right edge are meridians on
# their way to the back.
BOTTOM = [77.95, 78.43, 78.78, 79.29, 80.06, 81.18, 82.24, 83.49, 85.53, 87.93, 88.93,
          91.46, 91.87, 94.90, 95.20, 97.42, 99.96, 100.43, 102.54, 104.45, 106.75,
          108.75, 110.75]
RIGHT = [74.32, 74.50, 74.68, 75.23, 75.62, 76.33, 77.42, 78.84, 82.22, 85.94, 90.39,
         95.06, 99.41, 103.26, 105.89]
MERIDIAN_EXITS = [82.83, 91.91, 99.20, 105.48]
OUTLINE_LEAN = 0.24

FIX = 16                                   # 1/16 px


def unit(v):
    v = np.asarray(v, float)
    return v / math.hypot(*v)


def bez(p0, p1, p2, p3, n):
    t = np.linspace(0, 1, n)[:, None]
    return (1 - t) ** 3 * p0 + 3 * (1 - t) ** 2 * t * p1 + 3 * (1 - t) * t * t * p2 + t ** 3 * p3


# The skull's outline: from the forehead on the bottom edge up over the
# crown to the right edge (on the panel it runs on to the back).
S0 = np.array([BOTTOM[0], BOT])
S3 = np.array([RGT, RIGHT[0]])
_l = 0.40 * math.hypot(*(S3 - S0))
N_SIL = 800
SIL = bez(S0, S0 + 0.75 * _l * unit((-OUTLINE_LEAN, -1)), S3 - _l * unit((1, 0.10)), S3, N_SIL)
K = np.array([111.6, 106.6])  # the rings' centre, just past the corner


def ring_point(s, t):
    """Point t (0 forehead .. 1 right edge) of the ring at scale s."""
    i = np.clip(t, 0, 1) * (N_SIL - 1)
    i0 = np.floor(i).astype(int)
    i1 = np.minimum(i0 + 1, N_SIL - 1)
    f = (i - i0)[:, None]
    p = SIL[i0] * (1 - f) + SIL[i1] * f
    return K + np.asarray(s, float)[:, None] * (p - K)


def inside(p):
    return p[(p[:, 1] <= BOT + 0.02) & (p[:, 0] <= RGT + 0.02)]


def head_lines():
    """The head's lines in mm: (points, width)."""
    lines = [(SIL, OUTLINE_W)]
    used = set()
    bottom = BOTTOM[1:]
    for y in RIGHT[1:]:
        s = (K[1] - y) / (K[1] - S3[1])
        t = np.linspace(0, 1, 500)
        ring = ring_point(np.full(500, s), t)
        xb = K[0] - s * (K[0] - S0[0])
        near = [b for b in bottom if abs(b - xb) < 1.0 and b not in used]
        if near:	# end on the panel's line nearest to where it would
            b = min(near, key=lambda b: abs(b - xb))
            used.add(b)
            ring[:, 0] += (b - xb) * (1 - t) ** 2
        lines.append((inside(ring), LINE_W))
    # meridians: the other crossings, right to left, to the exits on the
    # right edge (lowest first), then along the outline toward the forehead
    starts = sorted([b for b in bottom if b not in used], reverse=True)
    ends = [((K[1] - y) / (K[1] - S3[1]), 1.0) for y in sorted(MERIDIAN_EXITS, reverse=True)]
    for te in np.linspace(0.90, 0.22, max(len(starts) - len(ends), 0)):
        ends.append((1.0, te))
    for x, (s1, t1) in zip(starts, ends):
        s0 = (K[0] - x) / (K[0] - S0[0])
        u = np.linspace(0, 1, 400)
        t = t1 * u ** (1 / 1.6)		# up along the ring first,
        s = s0 + (s1 - s0) * u ** 1.6	# then out across the rings
        lines.append((inside(ring_point(s, t)), LINE_W))
    return lines


def to_px(p):
    return np.c_[(p[:, 0] - AA0[0]) * PX, (p[:, 1] - AA0[1]) * PX]


def moon_px():
    return (MOON_C[0] - AA0[0]) * PX, (MOON_C[1] - AA0[1]) * PX


def simplify(p, eps):
    """Ramer-Douglas-Peucker: keep the points that matter within eps."""
    if len(p) < 3:
        return p
    a, b = p[0], p[-1]
    ab = b - a
    n = math.hypot(*ab)
    if n == 0:
        d = np.hypot(*(p - a).T)
    else:
        d = np.abs(ab[0] * (p[:, 1] - a[1]) - ab[1] * (p[:, 0] - a[0])) / n
    i = int(np.argmax(d))
    if d[i] <= eps:
        return np.array([a, b])
    return np.vstack([simplify(p[:i + 1], eps)[:-1], simplify(p[i:], eps)])


def scene():
    """Everything in panel px: circles (x, y, r, width; width 0 = filled)
    and the head's polylines (points, width)."""
    mx, my = moon_px()
    moon = (mx, my, MOON_R * PX, 0)
    orbits = [(mx, my, (MOON_R - MOON_EDGE / 2) * PX, MOON_EDGE * PX)]
    orbits += [(mx, my, r * PX, w * PX) for r, w in ORBITS]
    head = [(simplify(to_px(p), 0.12), w * PX) for p, w in head_lines() if len(p) > 1]
    assert all(len(p) <= 64 for p, _ in head), "home.c's scene_lines() takes 64 points a line"
    return moon, orbits, head


def fx(v):
    return int(round(v * FIX))


def header():
    moon, orbits, head = scene()
    circ = lambda c: "{ %d, %d, %d, %d }" % tuple(fx(v) for v in c)
    o = ["/* Generated by libwb/gen/scene.py: do not edit. */",
         "/*",
         " * The frontpanel's moon, orbits and head, continued on the screen.",
         " * Panel pixels (rotation 0), in 1/16 px. A circle is",
         " * { x, y, radius, line width }, width 0: filled.",
         " */",
         "#pragma once",
         "",
         "static const short scene_moon[4] = %s;" % circ(moon),
         "/* the moon's edge and the three orbits */",
         "static const short scene_orbits[%d][4] = {" % len(orbits),
         "\t" + ",\n\t".join(circ(c) for c in orbits),
         "};",
         "",
         "/* the head: per line its number of points and its width, then the",
         " * points as x, y; 0 ends the list */",
         "static const short scene_head[] = {"]
    for p, w in head:
        o.append("\t%d, %d," % (len(p), fx(w)))
        flat = [fx(v) for xy in p for v in xy]
        for k in range(0, len(flat), 12):
            o.append("\t\t" + ", ".join(str(v) for v in flat[k:k + 12]) + ",")
    o.append("\t0")
    o.append("};")
    o.append("")
    return "\n".join(o)


# --- preview -------------------------------------------------------------

SS = 4


def render_scene():
    """The scene in white on the void, as the screen shows it (rotation 0)."""
    moon, orbits, head = scene()
    layers = []
    for items, opa in (([moon], 0.22), (orbits, 0.7), (head, 0.7)):
        im = Image.new("L", (W * SS, H * SS), 0)
        d = ImageDraw.Draw(im)
        for it in items:
            if len(it) == 4:
                x, y, r, w = it
                if w:
                    r += w / 2
                    d.ellipse([(x - r) * SS, (y - r) * SS, (x + r) * SS, (y + r) * SS],
                              outline=255, width=max(1, round(w * SS)))
                else:
                    d.ellipse([(x - r) * SS, (y - r) * SS, (x + r) * SS, (y + r) * SS], fill=255)
            else:
                p, w = it
                d.line([(a * SS, b * SS) for a, b in p], fill=255, width=max(1, round(w * SS)),
                       joint="curve")
        layers.append((np.array(im.resize((W, H), Image.LANCZOS)) / 255.0, opa))
    scr = np.zeros((H, W, 3))
    scr[:] = (10, 10, 20)
    for a, opa in layers:
        a = a[..., None] * opa
        scr = scr * (1 - a) + 235 * a
    return scr


def sexp(s):
    tok = re.compile(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()]+')
    stack = [[]]
    for m in tok.finditer(s):
        t = m.group(0)
        if t == "(":
            stack.append([])
        elif t == ")":
            x = stack.pop()
            stack[-1].append(x)
        else:
            stack[-1].append(t.strip('"'))
    return stack[0][0]


def polys(pcb, layer):
    for it in sexp(open(pcb).read()):
        if not (isinstance(it, list) and it and it[0] == "gr_poly"):
            continue
        if next(x[1] for x in it if isinstance(x, list) and x[0] == "layer") != layer:
            continue
        pts = next(x for x in it if isinstance(x, list) and x[0] == "pts")
        yield [(float(p[1]), float(p[2])) for p in pts if p[0] == "xy"]


def raster(pcb, layer, x0, y0, x1, y1, s):
    """A layer's polygons at s px/mm, as 0..1."""
    im = Image.new("L", (int((x1 - x0) * s), int((y1 - y0) * s)), 0)
    d = ImageDraw.Draw(im)
    for p in polys(pcb, layer):
        d.polygon([((x - x0) * s, (y - y0) * s) for x, y in p], fill=255)
    return np.array(im) / 255.0


def preview(path, pcb, screen):
    scr = np.array(Image.open(screen).convert("RGB")).astype(float) if screen else render_scene()
    if not pcb:
        Image.fromarray(scr.astype(np.uint8)).save(path)
        return
    x0, y0, x1, y1 = 28.5, 25.0, 121.5, 124.0
    silk = raster(pcb, "F.SilkS", x0, y0, x1, y1, PX * 2)
    gold = raster(pcb, "F.Mask", x0, y0, x1, y1, PX * 2)
    w, h = silk.shape[1] // 2, silk.shape[0] // 2
    silk = np.array(Image.fromarray((silk * 255).astype(np.uint8)).resize((w, h), Image.LANCZOS)) / 255.0
    gold = np.array(Image.fromarray((gold * 255).astype(np.uint8)).resize((w, h), Image.LANCZOS)) / 255.0
    img = np.zeros((h, w, 3))
    img[:] = (16, 16, 18)                      # black solder mask
    img = img * (1 - gold[..., None]) + np.array((196, 160, 70)) * gold[..., None]
    img = img * (1 - silk[..., None]) + 235 * silk[..., None]
    cx0, cy0 = int((CUT[0] - x0) * PX), int((CUT[1] - y0) * PX)
    cx1, cy1 = int((CUT[2] - x0) * PX), int((CUT[3] - y0) * PX)
    img[cy0:cy1, cx0:cx1] = (4, 4, 6)          # the panel's black border
    ax, ay = round((AA0[0] - x0) * PX), round((AA0[1] - y0) * PX)
    img[ay:ay + H, ax:ax + W] = scr
    Image.fromarray(img.astype(np.uint8)).save(path)


# --- measuring ---------------------------------------------------------------

def measure(pcb):
    """Print the circles and the crossings this file has as constants."""
    s, x0, y0 = 20.0, 26.0, 22.0
    a = raster(pcb, "F.SilkS", x0, y0, 124.0, 180.0, s)
    ys, xs = np.nonzero(a > 0.5)
    bx, by = x0 + (xs + 0.5) / s, y0 + (ys + 0.5) / s
    # the moon's art outside the window frame (its lines at x 37.7-38.1, y 28.65-29.05)
    out = ~((bx > 37.6) & (bx < 112.7) & (by > 28.6) & (by < 107.7)) & (bx < 100)
    best = None
    for cx in np.arange(MOON_C[0] - 1, MOON_C[0] + 1, 0.05):
        for cy in np.arange(MOON_C[1] - 1, MOON_C[1] + 1, 0.05):
            r = np.hypot(bx[out] - cx, by[out] - cy)
            h, _ = np.histogram(r[(r > 22.6) & (r < 60)], bins=np.arange(22.6, 60, 0.05))
            score = float((h.astype(float) ** 2).sum())
            if best is None or score > best[0]:
                best = (score, cx, cy)
    _, cx, cy = best
    r = np.hypot(bx[out] - cx, by[out] - cy)
    h, e = np.histogram(r, bins=np.arange(15, 60, 0.05))
    print("moon centre (%.2f, %.2f)" % (cx, cy))
    fill = (e[:-1] > 20) & (e[:-1] < 22.5)
    full = h[fill].max()
    print("moon radius %.2f" % e[:-1][fill][h[fill] > full / 2][-1])
    ring = (e[:-1] > 22.5) & (h > 100)
    i = 0
    idx = np.nonzero(ring)[0]
    while i < len(idx):	# runs of bins: an orbit each
        j = i
        while j + 1 < len(idx) and idx[j + 1] == idx[j] + 1:
            j += 1
        b = idx[i:j + 1]
        print("orbit radius %.2f" % ((e[b] * h[b]).sum() / h[b].sum() + 0.025))
        i = j + 1

    def centres(v, start, step):
        on, out, i = v > 0.5, [], 0
        while i < len(on):
            if on[i]:
                j = i
                while j < len(on) and on[j]:
                    j += 1
                w = v[i:j]
                out.append(start + step * float((np.arange(i, j) * w).sum() / w.sum()))
                i = j
            else:
                i += 1
        return out

    def sample(x, y):
        fx_, fy_ = (x - x0) * s - 0.5, (y - y0) * s - 0.5
        i, j = np.floor(fx_).astype(int), np.floor(fy_).astype(int)
        u, v = fx_ - i, fy_ - j
        return (a[j, i] * (1 - u) * (1 - v) + a[j, i + 1] * u * (1 - v) +
                a[j + 1, i] * (1 - u) * v + a[j + 1, i + 1] * u * v)

    def track(lines):
        tr = [[c] for c in lines[0]]
        for row in lines[1:]:
            for t in tr:
                seen = [c for c in t if c is not None]
                pred = seen[-1] if len(seen) < 2 else 2 * seen[-1] - seen[-2]
                c = min(row, key=lambda c: abs(c - pred)) if row else None
                t.append(c if c is not None and abs(c - pred) < 0.35 else None)
        return tr

    step = 0.01
    xs_ = np.arange(76.5, RGT, step)
    rows = [106.02, 106.20, 106.38, 106.56]	# between the cut-out and the window frame
    lines = [centres(sample(xs_, np.full_like(xs_, y)), 76.5, step) for y in rows]
    print("bottom edge: x at the cut-out, dx per mm going down (lines leaning)")
    for t in track(lines):
        pts = [(r, c) for r, c in zip(rows, t) if c is not None]
        if len(pts) >= 3:
            k = np.polyfit(*zip(*pts), 1)
            print("  (%.2f, %.2f)" % (np.polyval(k, BOT), k[0]))
        else:
            print("  (%.2f, lost: a steep line)" % t[0])
    ys_ = np.arange(70, BOT, step)
    cols = [111.02, 111.20, 111.38, 111.56]
    lines = [centres(sample(np.full_like(ys_, x), ys_), 70, step) for x in cols]
    print("right edge: y at the cut-out, dy per mm going right")
    for t in track(lines):
        pts = [(c_, c) for c_, c in zip(cols, t) if c is not None]
        if len(pts) >= 3:
            k = np.polyfit(*zip(*pts), 1)
            print("  (%.2f, %.2f)" % (np.polyval(k, RGT), k[0]))
        else:
            print("  (%.2f, lost: a steep line)" % t[0])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview")
    ap.add_argument("--pcb")
    ap.add_argument("--screen", help="with --preview: this 720x720 picture on the screen")
    ap.add_argument("--measure", metavar="PCB")
    args = ap.parse_args()
    if args.measure:
        measure(args.measure)
    elif args.preview:
        preview(args.preview, args.pcb, args.screen)
    else:
        sys.stdout.write(header())


if __name__ == "__main__":
    main()
