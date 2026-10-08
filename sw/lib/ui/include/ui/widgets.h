/*
 * Reusable screens: menu, value slider and scrollable text view.
 */

#pragma once

#include "ui/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* Menu ------------------------------------------------------------------ */

struct ui_menu;

struct ui_menu_item {
	const char *label;
	/* Pushed when the item is selected, or NULL. */
	struct ui_screen *submenu;
	/* Called when the item is selected and there is no submenu. */
	void (*action)(struct ui_menu *m, int idx);
};

struct ui_menu {
	struct ui_screen screen;
	const struct ui_menu_item *items;
	uint8_t count;
	uint8_t sel;
	uint8_t top;
	/* Optional: current value shown right-aligned, e.g. a setting. */
	const char *(*value)(struct ui_menu *m, int idx, char *buf, size_t len);
};

bool ui_menu_event(struct ui_screen *s, const struct ui_event *ev);
void ui_menu_draw(struct ui_screen *s, struct gfx *g);

/*
 * Wheel: move, wheel push / CALL: select, 1..9: select item n directly,
 * bend down: page down. Bend up and C go back (handled by ui_dispatch).
 */
#define UI_MENU(_title, _items, _value)                                                    \
	{                                                                                  \
		.screen = {.title = (_title), .event = ui_menu_event, .draw = ui_menu_draw},  \
		.items = (_items), .count = UI_ARRAY_SIZE(_items), .value = (_value),        \
	}

/* Slider ---------------------------------------------------------------- */

struct ui_slider;

struct ui_slider {
	struct ui_screen screen;
	int min, max, step;
	const char *unit;
	/* Live preview while turning the wheel. */
	void (*changed)(struct ui_slider *s, int value);
	/* accepted = false: cancelled with C, value is back at the initial one. */
	void (*done)(struct ui_slider *s, int value, bool accepted);
	int value;
	int initial;
};

bool ui_slider_event(struct ui_screen *s, const struct ui_event *ev);
void ui_slider_draw(struct ui_screen *s, struct gfx *g);
void ui_slider_open(struct ui_slider *sl, int value);

#define UI_SLIDER(_title, _min, _max, _step, _unit, _changed, _done)                       \
	{                                                                                  \
		.screen = {.title = (_title), .event = ui_slider_event,                    \
			   .draw = ui_slider_draw},                                         \
		.min = (_min), .max = (_max), .step = (_step), .unit = (_unit),              \
		.changed = (_changed), .done = (_done),                                    \
	}

/* Text view ------------------------------------------------------------- */

struct ui_textview {
	struct ui_screen screen;
	const char *text;
	int scroll;
	int lines;
};

void ui_textview_open(struct ui_textview *tv, const char *title, const char *text);
/* Show a text in a shared text view (for short notices). */
void ui_message(const char *title, const char *text);

#ifdef __cplusplus
}
#endif
