/*
 * Node list (most recently heard first) and node details. Push on a node
 * opens its details; push or CALL there writes a direct message.
 */

#include <stdio.h>
#include <stdlib.h>

#include <zephyr/sys/util.h>
#include <string.h>

#include "../i18n.h"
#include "../mesh/mesh.h"
#include "../clock.h"
#include "../gps.h"
#include "meshtastic/geo.h"
#include "meshtastic/nodedb.h"
#include "screens.h"

static int sel, top;
static uint32_t detail_num;
static int detail_top, detail_lines;

/* "5s", "12m", "3h", "2d", or "-" if not heard since the start. */
static void format_age(uint32_t last_heard, char *buf, size_t size)
{
	uint32_t now = mesh_uptime_s();
	uint32_t age = now - last_heard;

	if (last_heard == 0) {
		snprintf(buf, size, "-");
	} else if (age < 60) {
		snprintf(buf, size, "%us", age);
	} else if (age < 3600) {
		snprintf(buf, size, "%um", age / 60);
	} else if (age < 86400) {
		snprintf(buf, size, "%uh", age / 3600);
	} else {
		snprintf(buf, size, "%ud", age / 86400);
	}
}

static int visible(void)
{
	return (GFX_HEIGHT - UI_CONTENT_Y - UI_TITLE_H) / UI_ROW_H;
}

static bool nodes_event(struct ui_screen *s, const struct ui_event *ev)
{
	int n;

	ARG_UNUSED(s);
	mesh_lock();
	n = mesh_nodes()->count;
	mesh_unlock();
	if (ev->type == UI_EV_WHEEL && n) {
		sel = ((sel - ev->value) % n + n) % n;
		ui_invalidate();
		return true;
	}
	if ((ui_ev_press(ev, UI_KEY_SELECT) || ui_ev_press(ev, UI_KEY_CALL)) && n) {
		uint8_t idx[MT_MAX_NODES];
		uint32_t num;

		mesh_lock();
		mt_nodedb_sorted(mesh_nodes(), idx, MT_MAX_NODES);
		num = mesh_nodes()->nodes[idx[sel < n ? sel : 0]].num;
		mesh_unlock();
		if (ui_ev_press(ev, UI_KEY_CALL)) {
			chat_open(num);
		} else {
			node_detail_open(num);
		}
		return true;
	}
	return false;
}

static void nodes_draw(struct ui_screen *s, struct gfx *g)
{
	uint8_t idx[MT_MAX_NODES];
	char title[24], name[40], age[8], tmp[16];
	int n, y0;

	ARG_UNUSED(s);
	mesh_lock();
	n = mt_nodedb_sorted(mesh_nodes(), idx, MT_MAX_NODES);
	snprintf(title, sizeof(title), "%s (%d)", tr("Nodes"), n);
	y0 = ui_draw_title(g, title);
	if (n == 0) {
		mesh_unlock();
		gfx_text_aligned(g, gfx_font_text, 0, y0 + 20, GFX_WIDTH, GFX_ALIGN_CENTER,
				 tr(mesh_available() ? "No nodes heard yet" : "Radio not available"),
				 GFX_DARK);
		return;
	}
	if (sel >= n) {
		sel = n - 1;
	}
	if (sel < top) {
		top = sel;
	} else if (sel >= top + visible()) {
		top = sel - visible() + 1;
	}
	for (int i = 0; i < visible() && top + i < n; i++) {
		const struct mt_node *node = &mesh_nodes()->nodes[idx[top + i]];
		int y = y0 + i * UI_ROW_H;
		bool on = top + i == sel;
		enum gfx_color fg = on ? GFX_WHITE : GFX_BLACK;

		snprintf(name, sizeof(name), "%s", mt_node_name(node, tmp, sizeof(tmp)));
		format_age(node->last_heard, age, sizeof(age));
		if (on) {
			gfx_fill_rect(g, 0, y, GFX_WIDTH, UI_ROW_H, GFX_BLACK);
		}
		int aw = gfx_text_width(gfx_font_small, age, -1);

		gfx_text(g, gfx_font_small, GFX_WIDTH - 3 - aw, y + 2, age, on ? GFX_LIGHT : GFX_DARK);
		gfx_text_fit(g, gfx_font_text, 2, y + 1, GFX_WIDTH - 8 - aw, name, fg);
	}
	mesh_unlock();
	ui_draw_scrollbar(g, y0, GFX_HEIGHT - y0, top, visible(), n);
}

struct ui_screen nodes_screen = {
	.title = "Nodes",
	.event = nodes_event,
	.draw = nodes_draw,
};

/* Details ------------------------------------------------------------------- */

static void detail_draw(struct ui_screen *s, struct gfx *g)
{
	char lines[8][32];
	int n = 0;
	char tmp[16], age[8];

	ARG_UNUSED(s);
	mesh_lock();
	struct mt_node *node = mt_nodedb_find(mesh_nodes(), detail_num);

	if (!node) {
		mesh_unlock();
		ui_draw_title(g, "?");
		return;
	}
	struct mt_node copy = *node;

	mesh_unlock();

	int y = ui_draw_title(g, mt_node_name(&copy, tmp, sizeof(tmp)));

	snprintf(lines[n++], 32, "!%08x %s", copy.num,
		 copy.has_user ? copy.user.short_name : "");
	format_age(copy.last_heard, age, sizeof(age));
	snprintf(lines[n++], 32, "%s: %s", tr("Heard"), age);
	if (copy.last_heard) {
		snprintf(lines[n++], 32, "SNR %d dB, %d dBm", copy.snr, copy.rssi);
		snprintf(lines[n++], 32, "%s: %u", tr("Hops"), copy.hops_away);
	}
	if (copy.has_position) {
		const struct mt_position *p = &copy.position;
		int32_t lat = p->latitude_i, lon = p->longitude_i;
		struct gps_fix me;

		/* Distance and direction from us, "~" when rounded by the sender. */
		if (gps_position(&me)) {
			uint32_t d = mt_geo_distance_m(me.lat_e7, me.lon_e7, lat, lon);
			int b = mt_geo_bearing_deg(me.lat_e7, me.lon_e7, lat, lon);
			const char *approx = p->precision_bits && p->precision_bits < 32 ? "~" : "";

			if (d < 1000) {
				snprintf(lines[n++], 32, "%s%u m %s", approx, d, mt_geo_compass(b));
			} else if (d < 100000) {
				snprintf(lines[n++], 32, "%s%u.%u km %s", approx, d / 1000,
					 d % 1000 / 100, mt_geo_compass(b));
			} else {
				snprintf(lines[n++], 32, "%s%u km %s", approx, d / 1000,
					 mt_geo_compass(b));
			}
		}
		snprintf(lines[n++], 32, "%s%d.%05d %s%d.%05d", lat < 0 ? "-" : "",
			 abs(lat) / 10000000, abs(lat) % 10000000 / 100, lon < 0 ? "-" : "",
			 abs(lon) / 10000000, abs(lon) % 10000000 / 100);
		/* Age of the position, if the sender's and our clock are set. */
		if (p->time && clock_valid() && clock_unix() >= p->time) {
			uint32_t a = clock_unix() - p->time;

			if (a < 60) {
				snprintf(age, sizeof(age), "%us", a);
			} else if (a < 3600) {
				snprintf(age, sizeof(age), "%um", a / 60);
			} else if (a < 86400) {
				snprintf(age, sizeof(age), "%uh", a / 3600);
			} else {
				snprintf(age, sizeof(age), "%ud", a / 86400);
			}
			snprintf(lines[n++], 32, "%s: %s", tr("Position"), age);
		}
	}
	snprintf(lines[n++], 32, "%s", tr(copy.has_user && copy.user.has_public_key
						  ? "Encrypted DMs"
						  : "No key (channel DMs)"));
	/* The wheel scrolls when the lines do not fit. */
	int rows = (GFX_HEIGHT - 8 - y) / 8;
	int y0 = y;

	detail_lines = n;
	detail_top = CLAMP(detail_top, 0, MAX(n - rows, 0));
	for (int i = detail_top; i < n && y + 8 <= GFX_HEIGHT - 8; i++, y += 8) {
		gfx_text_fit(g, gfx_font_text, 2, y + 1, GFX_WIDTH - 4, lines[i], GFX_BLACK);
	}
	ui_draw_scrollbar(g, y0, rows * 8, detail_top, rows, n);
	gfx_fill_rect(g, 0, GFX_HEIGHT - 8, GFX_WIDTH, 8, GFX_LIGHT);
	gfx_text_aligned(g, gfx_font_small, 0, GFX_HEIGHT - 7, GFX_WIDTH, GFX_ALIGN_CENTER,
			 tr("push: message"), GFX_BLACK);
}

static bool detail_event(struct ui_screen *s, const struct ui_event *ev)
{
	ARG_UNUSED(s);
	if (ev->type == UI_EV_WHEEL) {
		detail_top = CLAMP(detail_top - ev->value, 0, MAX(detail_lines - 1, 0));
		ui_invalidate();
		return true;
	}
	if (ui_ev_press(ev, UI_KEY_SELECT) || ui_ev_press(ev, UI_KEY_CALL)) {
		chat_open(detail_num);
		return true;
	}
	return false;
}

static struct ui_screen detail_screen = {
	.event = detail_event,
	.draw = detail_draw,
};

void node_detail_open(uint32_t num)
{
	detail_num = num;
	detail_top = 0;
	ui_push(&detail_screen);
}
