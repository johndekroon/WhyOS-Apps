# Host test for pdfview (run in the build VM: needs python3-pil and the
# MuPDF source sdapps/build.sh fetched into ~/sdapps/src). MuPDF is built
# for the host with the badge's options (MUPDF_OPTS in ../../build.sh),
# and its mutool makes the test PDFs.
OVERLAY = ../../../patches/buildroot/overlay-src
MUPDF_SRC ?= $(HOME)/sdapps/src/mupdf-1.28.5-source
MUPDF_OPTS = HAVE_X11=no HAVE_GLUT=no HAVE_CURL=no html=no xps=no svg=no extract=no mujs=no \
	brotli=no tofu=yes tofu_cjk=yes XCFLAGS="-DFZ_ENABLE_ICC=0 -DFZ_ENABLE_JPX=0 \
	-DFZ_ENABLE_CBZ=0 -DFZ_ENABLE_IMG=0 -DTOFU_SIL -DFZ_ENABLE_HYPHEN=0 -DFZ_PLOTTERS_CMYK=0 \
	-DFZ_PLOTTERS_N=0 -DFZ_ENABLE_OCR_OUTPUT=0 -DFZ_ENABLE_ODT_OUTPUT=0"
M = out/mupdf
CFLAGS ?= -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined

test: pdfview-host
	python3 test_pdfview.py

$(M)/libmupdf.a:
	$(MAKE) -s -C $(MUPDF_SRC) libs tools build=release OUT=$(CURDIR)/$(M) $(MUPDF_OPTS)

pdfview-host: ../pdfview.c $(M)/libmupdf.a
	$(CC) $(CFLAGS) -DPDFVIEW_HOST -I$(OVERLAY) -I$(MUPDF_SRC)/include -o $@ ../pdfview.c \
		$(M)/libmupdf.a $(M)/libmupdf-third.a -lm

clean:
	rm -rf out pdfview-host

.PHONY: test clean
