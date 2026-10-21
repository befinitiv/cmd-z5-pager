/*
 * GPS status: receiver state, satellites, the last fix and its age. The
 * receiver stays on while this screen is open.
 */

#include <stdio.h>
#include <stdlib.h>

#include <zephyr/kernel.h>

#include "../gps.h"
#include "../i18n.h"
#include "../mesh/mesh.h"
#include "screens.h"

#define LINE_H 8

/* "52.52001 N" from degrees * 1e7. */
static void fmt_coord(char *buf, size_t size, int32_t v_e7, char pos, char neg)
{
	uint32_t a = abs(v_e7);

	snprintf(buf, size, "%u.%05u %c", a / 10000000, a % 10000000 / 100, v_e7 < 0 ? neg : pos);
}

static void fmt_age(char *buf, size_t size, uint32_t s)
{
	if (s < 60) {
		snprintf(buf, size, "%us", s);
	} else if (s < 3600) {
		snprintf(buf, size, "%um", s / 60);
	} else {
		snprintf(buf, size, "%uh", s / 3600);
	}
}

static void gps_draw(struct ui_screen *s, struct gfx *g)
{
	struct gps_status st;
	struct gps_config gc;
	struct gps_fix pos;
	char lines[5][32], tmp[16];
	int n = 0;
	int y = ui_draw_title(g, s->title);

	gps_status(&st);
	gps_get_config(&gc);

	/* State of the receiver. */
	if (gc.mode == GPS_MODE_FIXED) {
		snprintf(lines[n++], 32, "%s", tr("Fixed position"));
	} else if (!st.receiver) {
		snprintf(lines[n++], 32, "%s", tr("No GPS receiver"));
	} else if (!st.powered) {
		snprintf(lines[n++], 32, "%s", tr("Off"));
	} else if (st.fixed_now) {
		snprintf(lines[n++], 32, "%s, %u %s", tr("Fix"), st.sats, tr("sat."));
	} else {
		snprintf(lines[n++], 32, "%s %u:%02u, %u %s", tr("Searching"), st.on_s / 60,
			 st.on_s % 60, st.sats, tr("sat."));
	}

	if (gps_position(&pos)) {
		fmt_coord(lines[n++], 32, pos.lat_e7, 'N', 'S');
		fmt_coord(lines[n++], 32, pos.lon_e7, 'E', 'W');
		if (!pos.manual) {
			snprintf(lines[n++], 32, "%d m  HDOP %u.%u", pos.alt_m, pos.hdop_x10 / 10,
				 pos.hdop_x10 % 10);
			fmt_age(tmp, sizeof(tmp), mesh_uptime_s() - pos.uptime_s);
			if (pos.speed_kmh) {
				snprintf(lines[n++], 32, "%s %s, %u km/h", tr("Fix"), tmp, pos.speed_kmh);
			} else {
				snprintf(lines[n++], 32, "%s %s %s", tr("Fix"), tmp, tr("ago"));
			}
		}
	} else if (gc.mode != GPS_MODE_OFF) {
		snprintf(lines[n++], 32, "%s", tr("No position yet"));
	}
	for (int i = 0; i < n; i++) {
		gfx_text_fit(g, gfx_font_text, 2, y + 1 + i * LINE_H, GFX_WIDTH - 4, lines[i],
			     i == 0 ? GFX_DARK : GFX_BLACK);
	}
	gfx_fill_rect(g, 0, GFX_HEIGHT - 8, GFX_WIDTH, 8, GFX_LIGHT);
	gfx_text_aligned(g, gfx_font_small, 0, GFX_HEIGHT - 7, GFX_WIDTH, GFX_ALIGN_CENTER,
			 tr("push: settings"), GFX_BLACK);
}

static bool gps_event(struct ui_screen *s, const struct ui_event *ev)
{
	ARG_UNUSED(s);
	if (ui_ev_press(ev, UI_KEY_SELECT)) {
		ui_push(&position_menu.screen);
		return true;
	}
	return false;
}

/* The receiver runs while the screen is visible, also in periodic mode. */
static void gps_enter(struct ui_screen *s)
{
	ARG_UNUSED(s);
	gps_hold(true);
}

static void gps_leave(struct ui_screen *s)
{
	ARG_UNUSED(s);
	gps_hold(false);
}

struct ui_screen gps_screen = {
	.title = "GPS",
	.enter = gps_enter,
	.leave = gps_leave,
	.event = gps_event,
	.draw = gps_draw,
	.tick_redraw = true,
};
