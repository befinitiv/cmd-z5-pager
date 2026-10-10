/*
 * Diagnostics: battery state and current consumption, measured by the
 * nPM1300 (simulated in the emulator). Refreshed every second; the wheel
 * scrolls.
 */

#include <stdio.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "../battery.h"
#include "../i18n.h"
#include "screens.h"

#define LINE_H   8
#define MAX_ROWS 12

struct row {
	char label[16];
	char value[20];
};

static struct row rows[MAX_ROWS];
static int nrows, top;

/* "4.62 mA", "12.5 mA", "112 mA"; negative while charging. */
static void fmt_ma(char *buf, size_t size, int ua)
{
	const char *sign = ua < 0 ? "-" : "";
	int a = abs(ua);

	if (a < 10000) {
		snprintf(buf, size, "%s%d.%02d mA", sign, a / 1000, (a % 1000) / 10);
	} else if (a < 100000) {
		snprintf(buf, size, "%s%d.%d mA", sign, a / 1000, (a % 1000) / 100);
	} else {
		snprintf(buf, size, "%s%d mA", sign, a / 1000);
	}
}

static void fmt_duration(char *buf, size_t size, uint32_t min)
{
	if (min >= 24 * 60) {
		snprintf(buf, size, "%u d %u h", min / (24 * 60), (min / 60) % 24);
	} else if (min >= 60) {
		snprintf(buf, size, "%u h %02u min", min / 60, min % 60);
	} else {
		snprintf(buf, size, "%u min", min);
	}
}

static struct row *add(const char *label)
{
	struct row *r = &rows[nrows++];

	snprintf(r->label, sizeof(r->label), "%s", tr(label));
	r->value[0] = '\0';
	return r;
}

static void collect(void)
{
	static const char *const charge_names[] = {
		[BATTERY_DISCHARGING] = "Battery",
		[BATTERY_CHARGING] = "Charging",
		[BATTERY_FULL] = "Charged",
		[BATTERY_ERROR] = "Charger error",
	};
	struct battery_status b;
	struct row *r;
	uint32_t avg_min = 0;

	battery_get(&b);
	nrows = 0;
	if (!b.valid) {
		return;
	}

	r = add("Charge");
	if (b.soc >= 0) {
		snprintf(r->value, sizeof(r->value), "%d %%", b.soc);
	} else {
		snprintf(r->value, sizeof(r->value), "?");
	}
	r = add("Voltage");
	snprintf(r->value, sizeof(r->value), "%d.%02d V", b.voltage_mv / 1000,
		 (b.voltage_mv % 1000) / 10);
	r = add("Power");
	snprintf(r->value, sizeof(r->value), "%s%s", b.usb ? "USB, " : "",
		 tr(charge_names[b.charge]));
	r = add("Current");
	fmt_ma(r->value, sizeof(r->value), b.current_ua);

	/* The averages cover at most the time since the start. */
	r = &rows[nrows++];
	snprintf(r->label, sizeof(r->label), "Ø 1 min");
	fmt_ma(r->value, sizeof(r->value), b.avg_min_ua);
	avg_min = MIN(b.uptime_s, 3600) / 60;
	if (avg_min >= 2) {
		r = &rows[nrows++];
		if (avg_min >= 60) {
			snprintf(r->label, sizeof(r->label), "Ø 1 h");
		} else {
			snprintf(r->label, sizeof(r->label), "Ø %u min", avg_min);
		}
		fmt_ma(r->value, sizeof(r->value), b.avg_hour_ua);
	}

	r = add("Drawn");
	if (b.drawn_uah < 10000) {
		snprintf(r->value, sizeof(r->value), "%u.%02u mAh", b.drawn_uah / 1000,
			 (b.drawn_uah % 1000) / 10);
	} else {
		snprintf(r->value, sizeof(r->value), "%u mAh", b.drawn_uah / 1000);
	}
	if (b.tte_min >= 0) {
		r = add("Runtime");
		fmt_duration(r->value, sizeof(r->value), b.tte_min);
	} else if (b.ttf_min >= 0) {
		r = add("Full in");
		fmt_duration(r->value, sizeof(r->value), b.ttf_min);
	}
	r = add("Temperature");
	snprintf(r->value, sizeof(r->value), "%d.%d °C", b.temp_dc / 10, abs(b.temp_dc) % 10);
	r = add("Uptime");
	if (k_uptime_seconds() < 60) {
		snprintf(r->value, sizeof(r->value), "%u s", (uint32_t)k_uptime_seconds());
	} else {
		fmt_duration(r->value, sizeof(r->value), k_uptime_seconds() / 60);
	}
}

static int visible(int y0)
{
	return (GFX_HEIGHT - y0) / LINE_H;
}

static bool diag_event(struct ui_screen *s, const struct ui_event *ev)
{
	ARG_UNUSED(s);
	if (ev->type == UI_EV_TICK) {
		ui_invalidate();
		return false;
	}
	if (ev->type == UI_EV_WHEEL) {
		top = CLAMP(top - ev->value, 0, MAX(nrows - visible(UI_CONTENT_Y + UI_TITLE_H), 0));
		ui_invalidate();
		return true;
	}
	return false;
}

static void diag_draw(struct ui_screen *s, struct gfx *g)
{
	int y0 = ui_draw_title(g, s->title);
	int n;

	collect();
	if (nrows == 0) {
		gfx_text_aligned(g, gfx_font_text, 0, y0 + 20, GFX_WIDTH, GFX_ALIGN_CENTER,
				 tr("No battery data"), GFX_DARK);
		return;
	}
	n = visible(y0);
	top = CLAMP(top, 0, MAX(nrows - n, 0));
	for (int i = 0; i < n && top + i < nrows; i++) {
		const struct row *r = &rows[top + i];
		int y = y0 + 1 + i * LINE_H;
		int vw = gfx_text_width(gfx_font_text, r->value, -1);

		gfx_text_fit(g, gfx_font_text, 2, y, GFX_WIDTH - 8 - vw, r->label, GFX_DARK);
		gfx_text(g, gfx_font_text, GFX_WIDTH - 4 - vw, y, r->value, GFX_BLACK);
	}
	ui_draw_scrollbar(g, y0, GFX_HEIGHT - y0, top, n, nrows);
}

struct ui_screen diag_screen = {
	.title = "Diagnostics",
	.event = diag_event,
	.draw = diag_draw,
};
