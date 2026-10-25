#!/usr/bin/env python3
"""Host test for pdfview (../pdfview.c built with -DPDFVIEW_HOST).

PDFs made with MuPDF's mutool (out/mupdf/mutool create) in out/pdfs:
coloured bands where a page should show them, at the width, zoomed and as
a whole page; the pages with the arrows, by scrolling past a page's end,
and through Go to page; the page a file was left at; an image, text in
the base 14 fonts; a locked, a broken and a missing file. Screens in
out/*.ppm.
"""
import os
import subprocess
import tempfile
import unittest

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, "pdfview-host")
MUTOOL = os.path.join(HERE, "out", "mupdf", "mutool")
PDFS = os.path.join(HERE, "out", "pdfs")
TOP, VIEW_H = 48, 720 - 48 - 56
CONFIG = tempfile.mkdtemp()


def page(name, box, body, fonts=(), images=()):
    p = os.path.join(PDFS, name + ".txt")
    with open(p, "w") as f:
        f.write("%%%%MediaBox %s\n" % box)
        for k, v in fonts:
            f.write("%%%%Font %s %s\n" % (k, v))
        for k, v in images:
            f.write("%%%%Image %s %s\n" % (k, v))
        f.write(body)
    return p


def make(name, *pages):
    subprocess.run([MUTOOL, "create", "-o", os.path.join(PDFS, name), *pages], check=True)


def view(name, keys="", config=None):
    shot = os.path.join(HERE, "out", "%s-%s.ppm" % (name, keys or "open"))
    env = dict(os.environ, APP_CONFIG=config or tempfile.mkdtemp(), ASAN_OPTIONS="detect_leaks=0")
    r = subprocess.run([EXE, os.path.join(PDFS, name), shot, keys], capture_output=True, text=True, env=env)
    assert r.returncode == 0 and "runtime error" not in r.stderr, r.stderr
    out = dict(l.split(" ", 1) for l in r.stdout.strip().split("\n") if " " in l)
    out["error"] = out.get("error", "")
    for k in ("size", "view", "cache"):
        out[k] = tuple(map(int, out[k].split()))
    with Image.open(shot) as img:
        return out, img.copy()


def near(px, rgb, tol=40):
    return all(abs(a - b) <= tol for a, b in zip(px, rgb))


RED, BLUE, GREEN = (1, 0, 0), (0, 0, 1), (0, 0.6, 0)


def band(rgb, box_h):
    """A band 100 points high across the top of a page box_h high."""
    return "%g %g %g rg 0 %d 2000 100 re f\n" % (*rgb, box_h - 100)


def setUpModule():
    os.makedirs(PDFS, exist_ok=True)
    a4 = "0 0 595 842"
    text = "BT /F1 40 Tf 60 600 Td (Page %d) Tj ET\n"
    make("three.pdf",
         page("p1", a4, band(RED, 842) + "0 0 1 rg 0 0 595 100 re f\n" + text % 1, [("F1", "Helvetica")]),
         page("p2", a4, band(BLUE, 842) + text % 2, [("F1", "Helvetica")]),
         page("p3", "0 0 842 595", band(GREEN, 595) + text % 3, [("F1", "Helvetica")]))
    make("forty.pdf", *[page("f%d" % i, a4, text % i, [("F1", "Times-Roman")]) for i in range(1, 41)])
    lines = "".join("BT /F1 12 Tf 50 %d Td (The quick brown fox jumps over the lazy dog, line %d.) Tj ET\n"
                    % (800 - 14 * i, i) for i in range(50))
    make("text.pdf", page("t", a4, lines, [("F1", "Courier")]))
    jpg = os.path.join(PDFS, "pic.jpg")
    img = Image.new("RGB", (400, 300), (255, 160, 0))
    img.paste((0, 160, 255), (200, 0, 400, 300))
    img.save(jpg, quality=90)
    make("image.pdf", page("i", a4, "q 595 0 0 446 0 396 cm /Im1 Do Q\n", images=[("Im1", jpg)]))
    subprocess.run([MUTOOL, "clean", "-E", "aes-256", "-U", "secret", "-O", "owner",
                    os.path.join(PDFS, "three.pdf"), os.path.join(PDFS, "locked.pdf")], check=True)
    with open(os.path.join(PDFS, "broken.pdf"), "wb") as f:
        f.write(b"%PDF-1.7\n" + bytes(range(256)) * 20)
    open(os.path.join(PDFS, "empty.pdf"), "wb").close()


class Pages(unittest.TestCase):
    def test_width(self):
        r, img = view("three.pdf")
        self.assertEqual(r["page"], "1 of 3")
        self.assertEqual(r["size"], (720, 1019))           # 842 * 720 / 595
        self.assertEqual(r["error"], "")
        self.assertTrue(near(img.getpixel((360, TOP + 60)), (255, 0, 0)))     # the red band
        self.assertTrue(near(img.getpixel((360, TOP + 300)), (255, 255, 255)))
        self.assertEqual(r["cache"], (0, 0, 720, 1019))    # the whole page kept

    def test_next_and_previous(self):
        r, img = view("three.pdf", "r")
        self.assertEqual(r["page"], "2 of 3")
        self.assertTrue(near(img.getpixel((360, TOP + 60)), (0, 0, 255)))
        r, img = view("three.pdf", "rr")
        self.assertEqual((r["page"], r["size"]), ("3 of 3", (720, 509)))
        top = TOP + (VIEW_H - 509) // 2                    # a landscape page in the middle
        self.assertTrue(near(img.getpixel((360, top + 40)), (0, 153, 0)))
        self.assertTrue(near(img.getpixel((360, top - 10)), (10, 10, 20), 12))
        r, _ = view("three.pdf", "rrrr")
        self.assertEqual(r["page"], "3 of 3")              # no further
        r, _ = view("three.pdf", "l")
        self.assertEqual(r["page"], "1 of 3")

    def test_scroll_into_the_next_page(self):
        r, _ = view("three.pdf", "ddd")
        self.assertEqual((r["page"], r["view"]), ("1 of 3", (0, 192)))
        r, img = view("three.pdf", "D")                     # Fn+Down: a screen, here to the end
        self.assertEqual(r["view"], (0, 1019 - VIEW_H))
        self.assertTrue(near(img.getpixel((360, TOP + VIEW_H - 40)), (0, 0, 255)))   # the bottom's blue
        r, _ = view("three.pdf", "Dd")                      # Down at the end: the next page
        self.assertEqual((r["page"], r["view"]), ("2 of 3", (0, 0)))
        r, _ = view("three.pdf", "ru")                      # Up at the top: the previous page's end
        self.assertEqual((r["page"], r["view"]), ("1 of 3", (0, 1019 - VIEW_H)))
        r, _ = view("three.pdf", "s")                       # Space: a screen
        self.assertEqual(r["view"], (0, 1019 - VIEW_H))
        r, _ = view("three.pdf", "ss")
        self.assertEqual((r["page"], r["view"]), ("2 of 3", (0, 0)))
        r, _ = view("three.pdf", "ssS")                     # Shift+Space back: the previous page's end
        self.assertEqual((r["page"], r["view"]), ("1 of 3", (0, 1019 - VIEW_H)))

    def test_zoom(self):
        r, img = view("three.pdf", "e")
        self.assertEqual((r["zoom"], r["size"]), ("1", (1440, 2038)))
        self.assertEqual(r["view"], (360, 308))             # the view keeps its middle
        self.assertEqual(r["cache"][2], 720)                # too big: the view's width
        r, img = view("three.pdf", "euuuuu")
        self.assertEqual(r["view"], (360, 0))
        self.assertTrue(near(img.getpixel((360, TOP + 100)), (255, 0, 0)))
        r, _ = view("three.pdf", "errr")                    # zoomed: Right moves the page
        self.assertEqual((r["page"], r["view"]), ("1 of 3", (720, 308)))
        r, _ = view("three.pdf", "eddddddddddddddd")
        self.assertEqual(r["view"][1], 308 + 15 * 64)
        self.assertLessEqual(r["cache"][1], 308 + 15 * 64)
        r, _ = view("three.pdf", "ee")
        self.assertEqual((r["zoom"], r["size"]), ("0", (720, 1019)))

    def test_whole_page(self):
        r, img = view("three.pdf", "mde")
        self.assertEqual((r["whole"], r["size"]), ("1", (435, VIEW_H)))
        self.assertTrue(near(img.getpixel((360, TOP + 30)), (255, 0, 0)))
        self.assertTrue(near(img.getpixel((60, TOP + 30)), (10, 10, 20), 12))   # beside the page
        self.assertTrue(near(img.getpixel((360, TOP + VIEW_H - 30)), (0, 0, 255)))

    def test_go_to_page(self):
        r, _ = view("forty.pdf", "me35e")
        self.assertEqual((r["page"], r["pop"]), ("35 of 40", "0"))
        r, _ = view("forty.pdf", "me99e")
        self.assertEqual(r["page"], "1 of 40")              # out of range: stays
        r, img = view("forty.pdf", "me12b4", )
        self.assertEqual(r["pop"], "3")
        r, _ = view("forty.pdf", "me12b4e")
        self.assertEqual(r["page"], "14 of 40")

    def test_opens_where_it_was_left(self):
        cfg = tempfile.mkdtemp()
        view("forty.pdf", "me7e", config=cfg)
        view("three.pdf", "r", config=cfg)
        r, _ = view("forty.pdf", "", config=cfg)
        self.assertEqual(r["page"], "7 of 40")
        r, _ = view("three.pdf", "", config=cfg)
        self.assertEqual(r["page"], "2 of 3")
        with open(os.path.join(cfg, "places")) as f:
            self.assertEqual(f.read().split("\n")[0], "2\t" + os.path.join(PDFS, "three.pdf"))


class Content(unittest.TestCase):
    def test_image(self):
        r, img = view("image.pdf")
        y = TOP + int((842 - 396 - 446 / 2) * 720 / 595)     # the image's middle row
        self.assertTrue(near(img.getpixel((180, y)), (255, 160, 0)))
        self.assertTrue(near(img.getpixel((540, y)), (0, 160, 255)))

    def test_text(self):
        r, img = view("text.pdf")
        dark = sum(1 for y in range(TOP + 40, TOP + 400) for x in range(60, 660)
                   if sum(img.getpixel((x, y))) < 300)
        self.assertGreater(dark, 5000)                      # the lines drawn in Courier

    def test_help_and_close(self):
        r, _ = view("three.pdf", "h")
        self.assertEqual(r["pop"], "1")
        r, _ = view("three.pdf", "x")
        self.assertEqual(r["closed"], "1")


class Bad(unittest.TestCase):
    def test_locked(self):
        r, img = view("locked.pdf")
        self.assertEqual(r["error"], "This PDF is locked with a password")
        red = sum(1 for x in range(100, 620) if img.getpixel((x, TOP + VIEW_H // 2))[0] > 200)
        self.assertGreater(red, 0)                          # the message, in the error colour

    def test_broken(self):
        r, _ = view("broken.pdf")
        self.assertEqual(r["error"], "Not a PDF this viewer can read")

    def test_empty_and_missing(self):
        r, _ = view("empty.pdf")
        self.assertEqual(r["error"], "Not a PDF this viewer can read")
        r, _ = view("nothere.pdf")
        self.assertEqual(r["error"], "Cannot open the file")


if __name__ == "__main__":
    unittest.main()
