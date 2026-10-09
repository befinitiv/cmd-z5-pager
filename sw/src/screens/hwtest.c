/*
 * Hardware test: exercises every output and shows raw input events. Useful
 * for board bring-up and to check the emulator.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "../config.h"
#include "../hw.h"
#include "../i18n.h"
#include "screens.h"

/* Melody: start of "Für Elise". */
static const struct hw_note melody[] = {
	{659, 150}, {622, 150}, {659, 150}, {622, 150}, {659, 150}, {494, 150},
	{587, 150}, {523, 150}, {440, 400},
};

/* Key test ----------------------------------------------------------------- */

static const char *const key_names[UI_KEY_COUNT] = {
	[UI_KEY_NONE] = "-",       [UI_KEY_0] = "0",         [UI_KEY_1] = "1",
	[UI_KEY_2] = "2",          [UI_KEY_3] = "3",         [UI_KEY_4] = "4",
	[UI_KEY_5] = "5",          [UI_KEY_6] = "6",         [UI_KEY_7] = "7",
	[UI_KEY_8] = "8",          [UI_KEY_9] = "9",         [UI_KEY_STAR] = "*",
	[UI_KEY_POUND] = "#",      [UI_KEY_CALL] = "CALL",   [UI_KEY_MEMO] = "Memo",
	[UI_KEY_CLEAR] = "C",      [UI_KEY_SELECT] = "push", [UI_KEY_BACK] = "bend up",
	[UI_KEY_DOWN] = "bend dn", [UI_KEY_POWER] = "power",
};

static const char *const ev_names[] = {
	[UI_EV_KEY_PRESS] = "press",
	[UI_EV_KEY_LONG] = "long",
	[UI_EV_KEY_RELEASE] = "release",
	[UI_EV_WHEEL] = "wheel",
};

static struct ui_event last_ev = {.type = UI_EV_TICK};
static int wheel_total;
static int event_count;

static bool keytest_event(struct ui_screen *s, const struct ui_event *ev)
{
	ARG_UNUSED(s);

	if (ev->type == UI_EV_TICK) {
		return false;
	}
	/* Holding C or bend up leaves the screen; everything else is shown. */
	if (ui_ev_home(ev)) {
		ui_pop();
		return true;
	}
	if (ev->type == UI_EV_WHEEL) {
		wheel_total += ev->value;
	}
	last_ev = *ev;
	event_count++;
	ui_invalidate();
	return true;
}

static void keytest_draw(struct ui_screen *s, struct gfx *g)
{
	char buf[32];
	int y = ui_draw_title(g, s->title) + 2;

	if (last_ev.type <= UI_EV_WHEEL) {
		snprintf(buf, sizeof(buf), tr("Event: %s"), tr(ev_names[last_ev.type]));
		gfx_text(g, gfx_font_text, 2, y, buf, GFX_BLACK);
		if (last_ev.type == UI_EV_WHEEL) {
			snprintf(buf, sizeof(buf), tr("Value: %+d"), last_ev.value);
		} else {
			snprintf(buf, sizeof(buf), tr("Key: %s"), key_names[last_ev.key]);
		}
		gfx_text(g, gfx_font_text, 2, y + 9, buf, GFX_BLACK);
	} else {
		gfx_text(g, gfx_font_text, 2, y, tr("Press a key"), GFX_DARK);
	}
	snprintf(buf, sizeof(buf), tr("Wheel: %d"), wheel_total);
	gfx_text(g, gfx_font_text, 2, y + 18, buf, GFX_BLACK);
	snprintf(buf, sizeof(buf), tr("Events: %d"), event_count);
	gfx_text(g, gfx_font_text, 2, y + 27, buf, GFX_BLACK);
	gfx_text_aligned(g, gfx_font_small, 0, GFX_HEIGHT - 6, GFX_WIDTH, GFX_ALIGN_CENTER,
			 tr("hold C/bend up: exit"), GFX_DARK);
}

static struct ui_screen keytest = {
	.title = "Key test",
	.event = keytest_event,
	.draw = keytest_draw,
};

/* Display test ------------------------------------------------------------- */

#define PATTERNS 5
static int pattern;

static bool disptest_event(struct ui_screen *s, const struct ui_event *ev)
{
	ARG_UNUSED(s);

	if (ev->type == UI_EV_WHEEL) {
		pattern = ((pattern - ev->value) % PATTERNS + PATTERNS) % PATTERNS;
		ui_invalidate();
		return true;
	}
	if (ev->type == UI_EV_KEY_PRESS && ev->key != UI_KEY_CLEAR && ev->key != UI_KEY_BACK) {
		pattern = (pattern + 1) % PATTERNS;
		ui_invalidate();
		return true;
	}
	return false;
}

static void disptest_draw(struct ui_screen *s, struct gfx *g)
{
	ARG_UNUSED(s);

	switch (pattern) {
	case 0: /* the four gray levels */
		for (int i = 0; i < 4; i++) {
			gfx_fill_rect(g, i * 24, 0, 24, GFX_HEIGHT - 9, i);
		}
		gfx_text(g, gfx_font_text, 2, GFX_HEIGHT - 8, "gray levels 0-3", GFX_BLACK);
		break;
	case 1: /* checkerboard, shows pixel errors */
		for (int y = 0; y < GFX_HEIGHT; y++) {
			for (int x = 0; x < GFX_WIDTH; x++) {
				gfx_pixel(g, x, y, ((x ^ y) & 1) ? GFX_BLACK : GFX_WHITE);
			}
		}
		break;
	case 2:
		gfx_clear(g, GFX_BLACK);
		break;
	case 3: /* frame and diagonals: checks the edges and row addressing */
		gfx_rect(g, 0, 0, GFX_WIDTH, GFX_HEIGHT, GFX_BLACK);
		for (int i = 0; i < GFX_HEIGHT; i++) {
			gfx_pixel(g, i * GFX_WIDTH / GFX_HEIGHT, i, GFX_BLACK);
			gfx_pixel(g, GFX_WIDTH - 1 - i * GFX_WIDTH / GFX_HEIGHT, i, GFX_BLACK);
		}
		break;
	default: /* font sample */
		gfx_text(g, gfx_font_small, 1, 1, "4x6: The quick brown", GFX_BLACK);
		gfx_text(g, gfx_font_text, 1, 9, "5x7: Grüße, Straße!", GFX_BLACK);
		gfx_text(g, gfx_font_text, 1, 18, "ÄÖÜ äöü ß € @ # ?", GFX_BLACK);
		gfx_text(g, gfx_font_text, 1, 27, "Hello, how are you?", GFX_DARK);
		gfx_text(g, gfx_font_large, 1, 40, "12:34", GFX_BLACK);
		break;
	}
}

static struct ui_screen disptest = {
	.title = "Display test",
	.event = disptest_event,
	.draw = disptest_draw,
};

/* Menu --------------------------------------------------------------------- */

static void hwtest_action(struct ui_menu *m, int idx)
{
	ARG_UNUSED(m);

	switch (idx) {
	case 0:
		hw_backlight_blink(3);
		break;
	case 1:
		hw_vibrate(400);
		break;
	case 2:
		hw_tone(HW_SPEAKER, 1000, 500, app_config.volume);
		break;
	case 3:
		hw_tone(HW_EARPIECE, 1000, 500, app_config.volume);
		break;
	case 4:
		hw_melody(HW_SPEAKER, melody, ARRAY_SIZE(melody), app_config.volume);
		break;
	case 5:
		hw_melody(HW_EARPIECE, melody, ARRAY_SIZE(melody), app_config.volume);
		break;
	case 6:
		last_ev.type = UI_EV_TICK;
		wheel_total = 0;
		event_count = 0;
		ui_push(&keytest);
		break;
	case 7:
		pattern = 0;
		ui_push(&disptest);
		break;
	default:
		break;
	}
}

static const struct ui_menu_item hwtest_items[] = {
	{"Blink LEDs", NULL, hwtest_action},
	{"Vibrate", NULL, hwtest_action},
	{"Speaker 1 kHz", NULL, hwtest_action},
	{"Earpiece 1 kHz", NULL, hwtest_action},
	{"Melody speaker", NULL, hwtest_action},
	{"Melody earpiece", NULL, hwtest_action},
	{"Key test", NULL, hwtest_action},
	{"Display test", NULL, hwtest_action},
};

struct ui_menu hwtest_menu = UI_MENU("Hardware test", hwtest_items, NULL);
