/* SPDX-License-Identifier: MIT */
/* store: the full-screen UI (docs/client.md §4) comes in phase 2. */
#include "store.h"

int ui_main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	warn("the store's screens are not there yet: use the commands (store --help)");
	return 2;
}
