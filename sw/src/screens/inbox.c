/*
 * Conversation list: the primary channel first, then direct-message
 * conversations by their newest message, with unread counts.
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

static void refresh(void)
{
	msgstore_lock();
	nconvs = msgstore_conversations(convs, MAX_CONVS);
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

		if (convs[idx] == CONV_CHANNEL) {
			snprintf(name, sizeof(name), "#%s", mesh_channel_name());
		} else {
			mesh_node_name(convs[idx], name, sizeof(name));
		}
		if (on) {
			gfx_fill_rect(g, 0, y, GFX_WIDTH, UI_ROW_H, GFX_BLACK);
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
