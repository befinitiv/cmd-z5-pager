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
extern struct ui_screen compose_screen;
extern struct ui_screen inbox_screen;
extern struct ui_screen nodes_screen;
extern struct ui_menu mesh_menu;

/* Write a message to a node, or to the channel (MT_BROADCAST). */
void compose_open_message(uint32_t to);
/* Edit a text setting; done() gets the result (cut to max_len bytes). */
void compose_open_edit(const char *title, const char *text, size_t max_len,
		       void (*done)(const char *text));
/* Conversation with a node, or the channel (CONV_CHANNEL). */
void chat_open(uint32_t conv);
void node_detail_open(uint32_t num);

void about_show(void);

/* Power key held: ask to release it; then the goodbye message. */
void poweroff_show(void);
void poweroff_goodbye(void);
