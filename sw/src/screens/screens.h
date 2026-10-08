/*
 * Application screens.
 */

#pragma once

#include "ui/ui.h"
#include "ui/widgets.h"

void statusbar_draw(struct gfx *g);

extern struct ui_screen home_screen;
extern struct ui_menu main_menu;
extern struct ui_menu settings_menu;
extern struct ui_menu hwtest_menu;
extern struct ui_menu messages_menu;
extern struct ui_screen compose_screen;

void about_show(void);

/* Power key held: ask to release it; then the goodbye message. */
void poweroff_show(void);
void poweroff_goodbye(void);
