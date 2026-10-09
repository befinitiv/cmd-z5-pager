/*
 * Idle screen: clock, date and a summary line. Wheel push or CALL opens the
 * main menu.
 */

#include <stdio.h>

#include "../clock.h"
#include "../i18n.h"
#include "../mesh/mesh.h"
#include "../mesh/msgstore.h"
#include "screens.h"

static const char *const weekdays[LANG_COUNT][7] = {
	[LANG_EN] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"},
	[LANG_DE] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"},
};
static const char *const months[LANG_COUNT][12] = {
	[LANG_EN] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov",
		     "Dec"},
	[LANG_DE] = {"Jan", "Feb", "Mär", "Apr", "Mai", "Jun", "Jul", "Aug", "Sep", "Okt", "Nov",
		     "Dez"},
};

static void home_draw(struct ui_screen *s, struct gfx *g)
{
	struct clock_tm tm;
	char buf[24];

	(void)s;
	if (clock_now(&tm)) {
		snprintf(buf, sizeof(buf), "%02d:%02d", tm.hour, tm.min);
		gfx_text_aligned(g, gfx_font_large, 0, 13, GFX_WIDTH, GFX_ALIGN_CENTER, buf,
				 GFX_BLACK);
		enum lang l = i18n_get();

		/* "Thu 8 Oct 2026" / "Do 8. Okt 2026" */
		snprintf(buf, sizeof(buf), l == LANG_DE ? "%s %d. %s %d" : "%s %d %s %d",
			 weekdays[l][tm.wday], tm.day, months[l][tm.month - 1], tm.year);
		gfx_text_aligned(g, gfx_font_text, 0, 35, GFX_WIDTH, GFX_ALIGN_CENTER, buf,
				 GFX_BLACK);
	} else {
		gfx_text_aligned(g, gfx_font_large, 0, 13, GFX_WIDTH, GFX_ALIGN_CENTER, "--:--",
				 GFX_DARK);
		gfx_text_aligned(g, gfx_font_text, 0, 35, GFX_WIDTH, GFX_ALIGN_CENTER,
				 tr("time not set"), GFX_DARK);
	}

	gfx_hline(g, 8, 46, GFX_WIDTH - 16, GFX_LIGHT);

	int unread, nodes;
	static struct msg newest;
	bool have_newest = false;

	msgstore_lock();
	unread = msgstore_unread(0);
	if (msgstore_newest_unread()) {
		newest = *msgstore_newest_unread();
		have_newest = true;
	}
	msgstore_unlock();
	mesh_lock();
	nodes = mesh_nodes()->count;
	mesh_unlock();

	if (have_newest) {
		/* Preview: sender and count, then the start of the newest unread text. */
		char who[24], count[16];

		mesh_node_name(newest.from, who, sizeof(who));
		snprintf(count, sizeof(count), tr("%d new"), unread);
		int cw = gfx_text_width(gfx_font_small, count, -1);

		gfx_text_fit(g, gfx_font_small, 2, 48, GFX_WIDTH - cw - 6, who, GFX_BLACK);
		gfx_text(g, gfx_font_small, GFX_WIDTH - 2 - cw, 48, count, GFX_BLACK);
		gfx_text_fit(g, gfx_font_text, 2, 55, GFX_WIDTH - 4, newest.text, GFX_BLACK);
	} else {
		if (!mesh_available()) {
			snprintf(buf, sizeof(buf), "%s", tr("Mesh off"));
		} else {
			snprintf(buf, sizeof(buf), tr(nodes == 1 ? "%d node" : "%d nodes"), nodes);
		}
		gfx_text_aligned(g, gfx_font_text, 0, 49, GFX_WIDTH, GFX_ALIGN_CENTER, buf,
				 GFX_DARK);
	}

	/* Soft key hint, unless the message preview needs the space. */
	if (!have_newest) {
		gfx_fill_rect(g, 0, GFX_HEIGHT - 8, GFX_WIDTH, 8, GFX_LIGHT);
		gfx_text_aligned(g, gfx_font_small, 0, GFX_HEIGHT - 7, GFX_WIDTH,
				 GFX_ALIGN_CENTER, tr("push: Menu"), GFX_BLACK);
	}
}

static bool home_event(struct ui_screen *s, const struct ui_event *ev)
{
	(void)s;
	if (ui_ev_press(ev, UI_KEY_SELECT) || ui_ev_press(ev, UI_KEY_MEMO)) {
		ui_push(&main_menu.screen);
		return true;
	}
	/* CALL: straight to the messages. */
	if (ui_ev_press(ev, UI_KEY_CALL)) {
		ui_push(&inbox_screen);
		return true;
	}
	return false;
}

struct ui_screen home_screen = {
	.title = "Home",
	.event = home_event,
	.draw = home_draw,
	.tick_redraw = true,
	.statusbar = true,
};
