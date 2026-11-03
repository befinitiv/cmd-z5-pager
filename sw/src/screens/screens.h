/*
 * Application screens.
 */

#pragma once

#include "ui/ui.h"
#include "ui/widgets.h"

void statusbar_draw(struct gfx *g);

extern struct ui_screen home_screen;
extern struct ui_menu main_menu;
extern struct ui_menu games_menu;
extern struct ui_menu settings_menu;
extern struct ui_menu hwtest_menu;
extern struct ui_screen compose_screen;
extern struct ui_screen inbox_screen;
extern struct ui_screen nodes_screen;
extern struct ui_menu mesh_menu;
extern struct ui_menu channels_menu;
extern struct ui_menu position_menu;
extern struct ui_screen gps_screen;
extern struct ui_screen diag_screen;
extern struct ui_screen noise_screen;

/* Write a message to a conversation: a node, or a channel (conv_of_channel()). */
void compose_open_message(uint32_t conv);
/* Edit a text setting; done() gets the result (cut to max_len bytes). */
void compose_open_edit(const char *title, const char *text, size_t max_len,
		       void (*done)(const char *text));
/* Conversation with a node, or the channel (CONV_CHANNEL). */
void chat_open(uint32_t conv);
/* True if the chat of this conversation is the current screen. */
bool chat_showing(uint32_t conv);
void node_detail_open(uint32_t num);

void about_show(void);

/* Jump and run game (in the games menu: the wheel bent down on the home screen). */
void game_open(void);
/* "Hexenflug": a witch evading bats, crows, towers and clouds. */
void witch_open(void);

/* Power key held: ask to release it; then the goodbye message. */
void poweroff_show(void);
void poweroff_goodbye(void);
/* Shown instead of the goodbye when the battery is empty. */
void poweroff_empty(void);
