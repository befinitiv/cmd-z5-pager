/*
 * Main menu and the About screen.
 */

#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/version.h>

#include "../app.h"
#include "../i18n.h"
#include "screens.h"

static void not_yet(struct ui_menu *m, int idx)
{
	static char text[96];

	snprintf(text, sizeof(text), tr("%s is not implemented yet. It follows in a later phase."),
		 tr(m->items[idx].label));
	ui_message(m->items[idx].label, text);
}

static void about_action(struct ui_menu *m, int idx)
{
	ARG_UNUSED(m);
	ARG_UNUSED(idx);
	about_show();
}

static const struct ui_menu_item main_items[] = {
	{"Messages", &inbox_screen, NULL},
	{"Nodes", &nodes_screen, NULL},
	{"GPS", NULL, not_yet},
	{"LoRaWAN", NULL, not_yet},
	{"Settings", &settings_menu.screen, NULL},
	{"Hardware test", &hwtest_menu.screen, NULL},
	{"About", NULL, about_action},
};

struct ui_menu main_menu = UI_MENU("Menu", main_items, NULL);

void about_show(void)
{
	static char text[160];

	snprintf(text, sizeof(text),
		 tr("CMD-Z5 firmware %s\nZephyr %s\nBuilt %s\n\n"
		    "Meshtastic and LoRaWAN messenger for the Sony CMD-Z5."),
		 APP_VERSION, KERNEL_VERSION_STRING, __DATE__);
	ui_message("About", text);
}
