/*
 * Status bar: battery, radio state and unread messages on the left, time on
 * the right.
 */

#include <stdio.h>

#include <zephyr/sys/util.h>

#include "battery.h"
#include "clock.h"
#include "mesh/mesh.h"
#include "mesh/msgstore.h"
#include "gfx/icons.h"
#include "screens/screens.h"

/* Lightning bolt right of the battery while USB power is present. */
static void draw_bolt(struct gfx *g, int x, int y)
{
	static const uint8_t px[][2] = {{2, 0}, {1, 1}, {0, 2}, {1, 2}, {2, 2}, {1, 3}, {0, 4}};

	for (size_t i = 0; i < ARRAY_SIZE(px); i++) {
		gfx_pixel(g, x + px[i][0], y + px[i][1], GFX_BLACK);
	}
}

static void draw_battery(struct gfx *g, int x, int y)
{
	struct battery_status b;

	battery_get(&b);
	gfx_rect(g, x, y, 11, 6, GFX_BLACK);
	gfx_vline(g, x + 11, y + 2, 2, GFX_BLACK);
	if (!b.valid || b.soc < 0) {
		gfx_text(g, gfx_font_small, x + 4, y, "?", GFX_DARK);
	} else {
		/* Round up, so a nearly empty battery still shows a pixel. */
		gfx_fill_rect(g, x + 2, y + 2, (7 * b.soc + 99) / 100, 2, GFX_BLACK);
	}
	if (b.valid && b.usb) {
		draw_bolt(g, x + 13, y);
	}
}

void statusbar_draw(struct gfx *g)
{
	struct clock_tm tm;
	char buf[8];

	draw_battery(g, 0, 0);
	/* Mesh radio: drawn light gray while it is not running. */
	gfx_draw_icon(g, 18, 0, &gfx_icon_antenna, mesh_available() ? GFX_BLACK : GFX_LIGHT);

	msgstore_lock();
	int unread = msgstore_unread(0);

	msgstore_unlock();
	if (unread) {
		gfx_draw_icon(g, 27, 0, &gfx_icon_envelope, GFX_BLACK);
		snprintf(buf, sizeof(buf), "%d", unread);
		gfx_text(g, gfx_font_small, 36, 0, buf, GFX_BLACK);
	}

	if (clock_now(&tm)) {
		snprintf(buf, sizeof(buf), "%02d:%02d", tm.hour, tm.min);
	} else {
		snprintf(buf, sizeof(buf), "--:--");
	}
	gfx_text_aligned(g, gfx_font_small, 0, 0, GFX_WIDTH, GFX_ALIGN_RIGHT, buf, GFX_BLACK);
}
