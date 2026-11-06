/*
 * Conversation list: the channels first (the primary one, then the private
 * ones), then direct-message conversations by their newest message, with
 * unread counts. Bending the wheel down opens a menu for the selected
 * conversation: silence it (marked with a crossed speaker) or delete its
 * messages (a channel stays in the list, empty).
 */

#include <stdio.h>

#include <zephyr/sys/util.h>

#include "../i18n.h"
#include "../mesh/mesh.h"
#include "../mesh/msgstore.h"
#include "gfx/icons.h"
#include "screens.h"

#define MAX_CONVS 16

static uint32_t convs[MAX_CONVS];
static int unread[MAX_CONVS];
static int nconvs;
static int sel, top;
static uint32_t chosen; /* the conversation of the menu */
static char menu_title[24];

static void refresh(void)
{
	uint32_t chans[MESH_CHANNELS] = {CONV_CHANNEL};
	int nchans = 1;
	struct mesh_channel c;

	for (int slot = 1; slot < MESH_CHANNELS; slot++) {
		if (mesh_get_channel(slot, &c)) {
			chans[nchans++] = conv_of_channel(slot);
		}
	}
	msgstore_lock();
	nconvs = msgstore_conversations(convs, MAX_CONVS, chans, nchans);
	for (int i = 0; i < nconvs; i++) {
		unread[i] = msgstore_unread(convs[i]);
	}
	msgstore_unlock();
	if (sel >= nconvs) {
		sel = nconvs - 1;
	}
}

static int visible(void)
{
	return (GFX_HEIGHT - UI_CONTENT_Y - UI_TITLE_H) / UI_ROW_H;
}

static void delete_yes(void)
{
	msgstore_lock();
	msgstore_delete_conv(chosen);
	msgstore_unlock();
	ui_pop(); /* the menu, back to the list */
	ui_invalidate();
}

static void conv_action(struct ui_menu *m, int idx)
{
	ARG_UNUSED(m);
	if (idx == 0) {
		if (!mesh_set_muted(chosen, !mesh_muted(chosen))) {
			ui_message("Silence", "Too many silenced conversations.");
		}
	} else {
		ui_confirm("Delete", "Delete all messages of this conversation?", delete_yes);
	}
}

static const char *conv_value(struct ui_menu *m, int idx, char *buf, size_t len)
{
	ARG_UNUSED(m);
	return idx == 0 ? tr(mesh_muted(chosen) ? "on" : "off") : NULL;
}

static const struct ui_menu_item conv_items[] = {
	{"Silence", NULL, conv_action},
	{"Delete messages", NULL, conv_action},
};

static struct ui_menu conv_menu = UI_MENU("", conv_items, conv_value);

static bool inbox_event(struct ui_screen *s, const struct ui_event *ev)
{
	ARG_UNUSED(s);
	if (ev->type == UI_EV_WHEEL && nconvs) {
		sel = ((sel - ev->value) % nconvs + nconvs) % nconvs;
		ui_invalidate();
		return true;
	}
	if ((ui_ev_press(ev, UI_KEY_SELECT) || ui_ev_press(ev, UI_KEY_CALL)) && nconvs) {
		chat_open(convs[sel]);
		return true;
	}
	if (ui_ev_press(ev, UI_KEY_DOWN) && nconvs) {
		chosen = convs[sel];
		mesh_conv_name(chosen, menu_title, sizeof(menu_title));
		conv_menu.screen.title = menu_title;
		conv_menu.sel = 0;
		conv_menu.top = 0;
		ui_push(&conv_menu.screen);
		return true;
	}
	return false;
}

static void inbox_draw(struct ui_screen *s, struct gfx *g)
{
	int y0 = ui_draw_title(g, s->title);
	char name[24], count[8];

	refresh();
	if (sel < top) {
		top = sel;
	} else if (sel >= top + visible()) {
		top = sel - visible() + 1;
	}
	for (int i = 0; i < visible() && top + i < nconvs; i++) {
		int idx = top + i;
		int y = y0 + i * UI_ROW_H;
		bool on = idx == sel;
		enum gfx_color fg = on ? GFX_WHITE : GFX_BLACK;
		int right = GFX_WIDTH - 2;

		mesh_conv_name(convs[idx], name, sizeof(name));
		if (on) {
			gfx_fill_rect(g, 0, y, GFX_WIDTH, UI_ROW_H, GFX_BLACK);
		}
		if (mesh_muted(convs[idx])) {
			right -= 9;
			gfx_draw_icon(g, right + 1, y + 2, &gfx_icon_muted, fg);
		}
		if (unread[idx]) {
			snprintf(count, sizeof(count), "%d", unread[idx]);
			right -= gfx_text_width(gfx_font_text, count, -1) + 2;
			gfx_text(g, gfx_font_text, right + 2, y + 1, count, fg);
			gfx_draw_icon(g, right - 8, y + 2, &gfx_icon_envelope, fg);
			right -= 10;
		}
		gfx_text_fit(g, gfx_font_text, 2, y + 1, right - 2, name, fg);
	}
	ui_draw_scrollbar(g, y0, GFX_HEIGHT - y0, top, visible(), nconvs);
}

struct ui_screen inbox_screen = {
	.title = "Messages",
	.event = inbox_event,
	.draw = inbox_draw,
};
