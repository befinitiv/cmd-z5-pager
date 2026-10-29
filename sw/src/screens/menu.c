/*
 * Main menu (the About screen is in about.c).
 */

#include <stdio.h>

#include <zephyr/kernel.h>

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

static void jump_action(struct ui_menu *m, int idx)
{
	ARG_UNUSED(m);
	ARG_UNUSED(idx);
	game_open();
}

static void witch_action(struct ui_menu *m, int idx)
{
	ARG_UNUSED(m);
	ARG_UNUSED(idx);
	witch_open();
}

static const struct ui_menu_item game_items[] = {
	{"Z5 Jump", NULL, jump_action},
	{"Hexenflug", NULL, witch_action},
};

static struct ui_menu games_menu = UI_MENU("Games", game_items, NULL);

static const struct ui_menu_item main_items[] = {
	{"Messages", &inbox_screen, NULL},
	{"Nodes", &nodes_screen, NULL},
	{"GPS", &gps_screen, NULL},
	{"LoRaWAN", NULL, not_yet},
	{"Settings", &settings_menu.screen, NULL},
	{"Games", &games_menu.screen, NULL},
	{"Diagnostics", &diag_screen, NULL},
	{"Hardware test", &hwtest_menu.screen, NULL},
	{"About", NULL, about_action},
};

struct ui_menu main_menu = UI_MENU("Menu", main_items, NULL);
