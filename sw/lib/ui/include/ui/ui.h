/*
 * Minimal retained-mode UI for the 96 x 72 display: a stack of screens that
 * receive key events and draw into a gfx framebuffer.
 *
 * Plain C without Zephyr dependencies. The application owns the event loop:
 * it feeds events with ui_dispatch() and calls ui_render() when
 * ui_needs_render() is true.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx/gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CONTAINER_OF
#define CONTAINER_OF(ptr, type, field) ((type *)(((char *)(ptr)) - offsetof(type, field)))
#endif

enum ui_key {
	UI_KEY_NONE,
	UI_KEY_0,
	UI_KEY_1,
	UI_KEY_2,
	UI_KEY_3,
	UI_KEY_4,
	UI_KEY_5,
	UI_KEY_6,
	UI_KEY_7,
	UI_KEY_8,
	UI_KEY_9,
	UI_KEY_STAR,
	UI_KEY_POUND,
	UI_KEY_CALL,
	UI_KEY_MEMO,
	UI_KEY_CLEAR,
	UI_KEY_SELECT, /* wheel pushed */
	UI_KEY_BACK,   /* wheel bent up: same role as C (back, long = home) */
	UI_KEY_DOWN,   /* wheel bent down */
	UI_KEY_POWER,
	UI_KEY_COUNT,
};

static inline bool ui_key_is_digit(enum ui_key k)
{
	return k >= UI_KEY_0 && k <= UI_KEY_9;
}

static inline int ui_key_digit(enum ui_key k)
{
	return k - UI_KEY_0;
}

enum ui_event_type {
	UI_EV_KEY_PRESS,   /* key went down */
	UI_EV_KEY_LONG,    /* key held for the long-press time */
	UI_EV_KEY_RELEASE, /* key went up; value = 1 if a LONG event was sent */
	UI_EV_WHEEL,       /* value = detents, positive = rotated up */
	UI_EV_TICK,        /* once per second */
};

struct ui_event {
	uint8_t type;
	uint8_t key;
	int16_t value;
};

/* A key release that was not preceded by a long press. */
static inline bool ui_ev_short(const struct ui_event *ev, enum ui_key k)
{
	return ev->type == UI_EV_KEY_RELEASE && ev->key == k && ev->value == 0;
}

static inline bool ui_ev_press(const struct ui_event *ev, enum ui_key k)
{
	return ev->type == UI_EV_KEY_PRESS && ev->key == k;
}

static inline bool ui_ev_long(const struct ui_event *ev, enum ui_key k)
{
	return ev->type == UI_EV_KEY_LONG && ev->key == k;
}

/* "Go back": short C or short wheel bend up. */
static inline bool ui_ev_back(const struct ui_event *ev)
{
	return ui_ev_short(ev, UI_KEY_CLEAR) || ui_ev_short(ev, UI_KEY_BACK);
}

/* "Go to the home screen": long C or long wheel bend up. */
static inline bool ui_ev_home(const struct ui_event *ev)
{
	return ui_ev_long(ev, UI_KEY_CLEAR) || ui_ev_long(ev, UI_KEY_BACK);
}

struct ui_screen {
	const char *title;
	/* Called when the screen becomes the top of the stack (push or uncover). */
	void (*enter)(struct ui_screen *s);
	/* Called when the screen stops being the top (covered or removed). */
	void (*leave)(struct ui_screen *s);
	/* Return true if the event was consumed. */
	bool (*event)(struct ui_screen *s, const struct ui_event *ev);
	/* Draw the content area (below the status bar). */
	void (*draw)(struct ui_screen *s, struct gfx *g);
	/* No status bar; draw() gets the whole display. */
	bool fullscreen;
	/* Optional: called on every pass of the main loop (a few times a second). */
	void (*poll)(struct ui_screen *s);
	/* Redraw on every tick (e.g. clocks). */
	bool tick_redraw;
};

/* Layout. */
#define UI_STATUS_H   7
#define UI_CONTENT_Y  (UI_STATUS_H + 1)
#define UI_CONTENT_H  (GFX_HEIGHT - UI_CONTENT_Y)
#define UI_TITLE_H    9
#define UI_ROW_H      9

#define UI_STACK_DEPTH 8

void ui_init(void (*statusbar)(struct gfx *g));

/*
 * Translation hook. Screen titles, menu labels and the widgets' own texts are
 * passed through it when drawn, so static tables can hold the English source
 * strings. Without a translator the strings are used as they are.
 */
void ui_set_translator(const char *(*tr)(const char *s));
const char *ui_tr(const char *s);

/* Replace the whole stack with this screen. */
void ui_set_root(struct ui_screen *s);
void ui_push(struct ui_screen *s);
void ui_pop(void);
/* Pop back to the root screen. */
void ui_home(void);
struct ui_screen *ui_top(void);

/*
 * Pass an event to the top screen. Unhandled: C or wheel bend up (short) pops
 * the screen, (long) returns to the root screen.
 */
void ui_dispatch(const struct ui_event *ev);

/* Call the top screen's poll hook. */
void ui_poll(void);

void ui_invalidate(void);
bool ui_needs_render(void);
/* Redraw everything into g. */
void ui_render(struct gfx *g);

/* Helpers for screens. */

/* Inverted title bar at the top of the content area. Returns the y below it. */
int ui_draw_title(struct gfx *g, const char *title);
/* Small scroll bar at the right edge of the area [y, y + h). */
void ui_draw_scrollbar(struct gfx *g, int y, int h, int first, int visible, int total);

#ifdef __cplusplus
}
#endif
