/*
 * Idle screen: clock, date and a summary line. Wheel push or CALL opens the
 * main menu.
 */

#include <stdio.h>

#include "../clock.h"
#include "../i18n.h"
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
	gfx_text_aligned(g, gfx_font_text, 0, 49, GFX_WIDTH, GFX_ALIGN_CENTER, tr("Mesh off"),
			 GFX_DARK);

	/* Soft key hint. */
	gfx_fill_rect(g, 0, GFX_HEIGHT - 8, GFX_WIDTH, 8, GFX_LIGHT);
	gfx_text_aligned(g, gfx_font_small, 0, GFX_HEIGHT - 7, GFX_WIDTH, GFX_ALIGN_CENTER,
			 tr("push: Menu"), GFX_BLACK);
}

static bool home_event(struct ui_screen *s, const struct ui_event *ev)
{
	(void)s;
	if (ui_ev_press(ev, UI_KEY_SELECT) || ui_ev_press(ev, UI_KEY_CALL) ||
	    ui_ev_press(ev, UI_KEY_MEMO)) {
		ui_push(&main_menu.screen);
		return true;
	}
	return false;
}

struct ui_screen home_screen = {
	.title = "Home",
	.event = home_event,
	.draw = home_draw,
	.tick_redraw = true,
};
