/* SPDX-License-Identifier: MIT */
/*
 * The WhyOS SDK: what an app on the badge is made of, apart from its own
 * logic. One header, with -I sdk/include and -I libwb:
 *
 *	#include "whyos.h"
 *
 *	whyos/core.h	the app's places, files, settings, log, clock, power
 *	whyos/keys.h	the keys by role, the characters they type
 *	libwb (wb.h)	drawing in the design system "Neon Orbit"
 *	whyos/ui.h	the app's key bar, a typed line, a card of lines
 *	whyos/app.h	screens, actions and settings; wos_run() runs the app
 *	whyos/test.h	the test channel the test framework uses
 *
 * Everything is static and header-only, as libwb is: a program links what
 * it uses (--gc-sections), and an app is one C file that includes this, or
 * several of which one draws. sdk/README.md is the guide.
 */
#pragma once

/* what an app does not use is not linked, and not worth a warning */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "whyos/core.h"
#include "whyos/keys.h"
#include "wb.h"
#include "whyos/ui.h"
#include "whyos/app.h"
#include "whyos/test.h"
#pragma GCC diagnostic pop
