/*
 * Status bar: battery, radio state and unread messages on the left, time on
 * the right. Battery and radio are placeholders until the PMIC and radio
 * services exist.
 */

#include <stdio.h>

#include "clock.h"
#include "gfx/icons.h"
#include "screens/screens.h"

/* -1 = unknown. Replaced by the fuel gauge in a later phase. */
static int battery_percent(void)
{
	return -1;
}

static void draw_battery(struct gfx *g, int x, int y)
{
	int pct = battery_percent();

	gfx_rect(g, x, y, 11, 6, GFX_BLACK);
	gfx_vline(g, x + 11, y + 2, 2, GFX_BLACK);
	if (pct < 0) {
		gfx_text(g, gfx_font_small, x + 4, y, "?", GFX_DARK);
	} else {
		gfx_fill_rect(g, x + 2, y + 2, 7 * pct / 100, 2, GFX_BLACK);
	}
}

void statusbar_draw(struct gfx *g)
{
	struct clock_tm tm;
	char buf[8];

	draw_battery(g, 0, 0);
	/* Mesh radio: drawn light gray while it is not running. */
	gfx_draw_icon(g, 16, 0, &gfx_icon_antenna, GFX_LIGHT);

	if (clock_now(&tm)) {
		snprintf(buf, sizeof(buf), "%02d:%02d", tm.hour, tm.min);
	} else {
		snprintf(buf, sizeof(buf), "--:--");
	}
	gfx_text_aligned(g, gfx_font_small, 0, 0, GFX_WIDTH, GFX_ALIGN_RIGHT, buf, GFX_BLACK);
}
