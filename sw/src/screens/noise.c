/*
 * Radio noise (Hardware test): the SX1262's noise floor while receiving, to
 * find parts of the device that disturb reception. Readings are averaged
 * per condition (GPS, backlight, display, USB power, supplies); a change of
 * condition starts a new average, and the one with the display off is kept
 * for comparison. Keys: 1 GPS on, 2 backlight off, 3 display off (any key
 * wakes it), 4 nRF52840 DC/DC or LDO, 5/6 forced PWM on the nPM1300's BUCK1
 * (radio) or BUCK2 (main rail), 7 the SX1262's own regulator DC/DC or LDO,
 * 8 the radio's reset line driven or left to its pull-up, 9 the RF switch's
 * receive path off (nothing is received meanwhile), 0 starts over
 * (not shown, the footer is full). The measurement goes on with the display off; leaving the
 * screen restores everything.
 */

#include <stdio.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "../battery.h"
#include "../gps.h"
#include "../hw.h"
#include "../i18n.h"
#include "../mesh/mesh.h"
#include "../power.h"
#include "screens.h"

#define SAMPLE_MS 250
#define LINE_H    8

enum {
	COND_GPS = BIT(0),
	COND_LIGHT = BIT(1),
	COND_DISPLAY = BIT(2),
	COND_USB = BIT(3),
	COND_DCDC = BIT(4),
	COND_PWM1 = BIT(5),
	COND_PWM2 = BIT(6),
	COND_RADIO_DCDC = BIT(7),
	COND_RESET_DRIVEN = BIT(8),
	COND_RX_PATH = BIT(9),
};

static bool gps_held, light_off;
static bool dcdc_default; /* the nRF52840's regulator when the screen opened */
static int64_t next_sample;
static int cond = -1;
static int16_t now_dbm;
static bool have_now;
/* Since the condition changed: sum and count. */
static int32_t sum;
static int count;
/* The last average with the display off (tenths of dB), if any. */
static int dark_avg_ddb;
static bool have_dark;

static int condition(void)
{
	struct gps_status g;
	struct battery_status b;
	int c = 0;

	gps_status(&g);
	battery_get(&b);
	c |= g.powered ? COND_GPS : 0;
	c |= power_backlight_on() ? COND_LIGHT : 0;
	c |= power_display_on() ? COND_DISPLAY : 0;
	c |= b.valid && b.usb ? COND_USB : 0;
	c |= hw_supply_dcdc_on() ? COND_DCDC : 0;
	c |= hw_supply_pwm_on(1) ? COND_PWM1 : 0;
	c |= hw_supply_pwm_on(2) ? COND_PWM2 : 0;
	c |= mesh_radio_dcdc_on() ? COND_RADIO_DCDC : 0;
	c |= mesh_radio_reset_driven() ? COND_RESET_DRIVEN : 0;
	c |= mesh_radio_rx_path_on() ? COND_RX_PATH : 0;
	return c;
}

static void restart(void)
{
	sum = 0;
	count = 0;
	have_now = false;
}

static int avg_ddb(void)
{
	return count ? (sum * 10 - count / 2) / count : 0;
}

static void noise_poll(struct ui_screen *s)
{
	int64_t t = k_uptime_get();
	int16_t dbm;
	int c;

	ARG_UNUSED(s);
	if (t < next_sample) {
		return;
	}
	next_sample = t + SAMPLE_MS;
	c = condition();
	if (c != cond) {
		if (cond >= 0 && !(cond & COND_DISPLAY) && count > 0) {
			dark_avg_ddb = avg_ddb();
			have_dark = true;
		}
		cond = c;
		restart();
		ui_invalidate();
	}
	if (mesh_noise_floor(&dbm) != 0) {
		return; /* sending or a packet arriving */
	}
	now_dbm = dbm;
	have_now = true;
	sum += dbm;
	count++;
	if (power_display_on()) {
		ui_invalidate();
	}
}

static void noise_enter(struct ui_screen *s)
{
	ARG_UNUSED(s);
	next_sample = 0;
	dcdc_default = hw_supply_dcdc_on();
}

static void noise_leave(struct ui_screen *s)
{
	ARG_UNUSED(s);
	/* Covered by a message or left: back to normal. */
	if (gps_held) {
		gps_held = false;
		gps_hold(false);
	}
	if (light_off) {
		light_off = false;
		power_block_backlight(false);
	}
	hw_supply_dcdc(dcdc_default);
	for (int buck = 1; buck <= 2; buck++) {
		if (hw_supply_pwm_on(buck)) {
			hw_supply_pwm(buck, false);
		}
	}
	if (!mesh_radio_dcdc_on()) {
		mesh_radio_dcdc(true);
	}
	if (!mesh_radio_reset_driven()) {
		mesh_radio_reset_line(true);
	}
	if (!mesh_radio_rx_path_on()) {
		mesh_radio_rx_path(true);
	}
	cond = -1;
}

static bool noise_event(struct ui_screen *s, const struct ui_event *ev)
{
	ARG_UNUSED(s);
	/* Off on the release of 3: the release itself would wake the display. */
	if (ev->type == UI_EV_KEY_RELEASE && ev->key == UI_KEY_3) {
		power_display_off();
		return true;
	}
	if (ev->type != UI_EV_KEY_PRESS) {
		return false;
	}
	switch (ev->key) {
	case UI_KEY_1:
		gps_held = !gps_held;
		gps_hold(gps_held);
		break;
	case UI_KEY_2:
		light_off = !light_off;
		power_block_backlight(light_off);
		break;
	case UI_KEY_3:
		return true; /* on release */
	case UI_KEY_4:
		hw_supply_dcdc(!hw_supply_dcdc_on());
		break;
	case UI_KEY_5:
	case UI_KEY_6: {
		int buck = ev->key == UI_KEY_5 ? 1 : 2;

		hw_supply_pwm(buck, !hw_supply_pwm_on(buck));
		break;
	}
	case UI_KEY_7:
		mesh_radio_dcdc(!mesh_radio_dcdc_on());
		break;
	case UI_KEY_8:
		mesh_radio_reset_line(!mesh_radio_reset_driven());
		break;
	case UI_KEY_9:
		mesh_radio_rx_path(!mesh_radio_rx_path_on());
		break;
	case UI_KEY_0:
		restart();
		have_dark = false;
		break;
	default:
		return false;
	}
	next_sample = 0;
	ui_invalidate();
	return true;
}

static void fmt_ddb(char *buf, size_t size, int ddb)
{
	snprintf(buf, size, "%s%d.%d dBm", ddb < 0 ? "-" : "", abs(ddb) / 10, abs(ddb) % 10);
}

static void row(struct gfx *g, int y, const char *label, const char *value)
{
	int vw = gfx_text_width(gfx_font_text, value, -1);

	gfx_text_fit(g, gfx_font_text, 2, y, GFX_WIDTH - 6 - vw, label, GFX_DARK);
	gfx_text(g, gfx_font_text, GFX_WIDTH - 2 - vw, y, value, GFX_BLACK);
}

static void noise_draw(struct ui_screen *s, struct gfx *g)
{
	int y = ui_draw_title(g, s->title) + 1;
	struct mesh_stats ms;
	char buf[24];

	if (!mesh_available()) {
		gfx_text_aligned(g, gfx_font_text, 0, y + 20, GFX_WIDTH, GFX_ALIGN_CENTER,
				 tr("No radio"), GFX_DARK);
		return;
	}
	if (have_now) {
		snprintf(buf, sizeof(buf), "%d dBm", now_dbm);
	} else {
		snprintf(buf, sizeof(buf), "-");
	}
	row(g, y, tr("Now"), buf);
	y += LINE_H;
	if (count > 0) {
		fmt_ddb(buf, sizeof(buf), avg_ddb());
	} else {
		snprintf(buf, sizeof(buf), "-");
	}
	row(g, y, tr("Average"), buf);
	y += LINE_H;
	if (have_dark) {
		fmt_ddb(buf, sizeof(buf), dark_avg_ddb);
		row(g, y, tr("LCD off"), buf);
		y += LINE_H;
	}
	mesh_get_stats(&ms);
	if (ms.last_rx_ms) {
		snprintf(buf, sizeof(buf), "%d/%+d dB", ms.last_rssi, ms.last_snr);
		row(g, y, tr("Last packet"), buf);
	}

	/* Conditions and the keys that change them. */
	snprintf(buf, sizeof(buf), "1 GPS %s 2 %s %s%s", tr((cond & COND_GPS) ? "on" : "off"),
		 tr("Light"), tr((cond & COND_LIGHT) ? "on" : "off"),
		 (cond & COND_USB) ? " USB" : "");
	gfx_text(g, gfx_font_small, 2, GFX_HEIGHT - 27, buf, GFX_DARK);
	snprintf(buf, sizeof(buf), "4 nRF:%s  7 SX:%s", (cond & COND_DCDC) ? "DC/DC" : "LDO",
		 (cond & COND_RADIO_DCDC) ? "DC/DC" : "LDO");
	gfx_text(g, gfx_font_small, 2, GFX_HEIGHT - 20, buf, GFX_DARK);
	snprintf(buf, sizeof(buf), "5 B1:%s  6 B2:%s", (cond & COND_PWM1) ? "PWM" : "auto",
		 (cond & COND_PWM2) ? "PWM" : "auto");
	gfx_text(g, gfx_font_small, 2, GFX_HEIGHT - 13, buf, GFX_DARK);
	snprintf(buf, sizeof(buf), "3 LCD 8 RST:%s 9 RX:%s",
		 (cond & COND_RESET_DRIVEN) ? "nRF" : "PU", (cond & COND_RX_PATH) ? tr("on") : "--");
	gfx_text(g, gfx_font_small, 2, GFX_HEIGHT - 6, buf, GFX_DARK);
}

struct ui_screen noise_screen = {
	.title = "Radio noise",
	.enter = noise_enter,
	.leave = noise_leave,
	.event = noise_event,
	.draw = noise_draw,
	.poll = noise_poll,
};
