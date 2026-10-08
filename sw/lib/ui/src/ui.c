#include "ui/ui.h"

static struct ui_screen *stack[UI_STACK_DEPTH];
static int depth;
static bool dirty = true;
static void (*draw_statusbar)(struct gfx *g);
/* Screen that received the press of each key. */
static struct ui_screen *press_screen[UI_KEY_COUNT];
static const char *(*translator)(const char *s);

void ui_set_translator(const char *(*tr)(const char *s))
{
	translator = tr;
	dirty = true;
}

const char *ui_tr(const char *s)
{
	return translator && s ? translator(s) : s;
}

void ui_init(void (*statusbar)(struct gfx *g))
{
	draw_statusbar = statusbar;
	depth = 0;
	dirty = true;
}

static void leaving(void)
{
	struct ui_screen *s = ui_top();

	if (s && s->leave) {
		s->leave(s);
	}
}

static void entered(void)
{
	struct ui_screen *s = ui_top();

	if (s && s->enter) {
		s->enter(s);
	}
	dirty = true;
}

void ui_set_root(struct ui_screen *s)
{
	leaving();
	stack[0] = s;
	depth = 1;
	entered();
}

void ui_push(struct ui_screen *s)
{
	leaving();
	if (depth == UI_STACK_DEPTH) {
		/* Drop the oldest non-root screen rather than failing. */
		for (int i = 1; i < depth - 1; i++) {
			stack[i] = stack[i + 1];
		}
		depth--;
	}
	stack[depth++] = s;
	entered();
}

void ui_pop(void)
{
	if (depth > 1) {
		leaving();
		depth--;
		entered();
	}
}

void ui_home(void)
{
	if (depth > 1) {
		leaving();
		depth = 1;
		entered();
	}
}

struct ui_screen *ui_top(void)
{
	return depth ? stack[depth - 1] : NULL;
}

void ui_dispatch(const struct ui_event *ev)
{
	struct ui_screen *s = ui_top();

	if (!s) {
		return;
	}
	if (ev->type == UI_EV_TICK && s->tick_redraw) {
		dirty = true;
	}
	/*
	 * A long press or release belongs to the screen that saw the press: a
	 * key that opened a screen must not act on the new screen too.
	 */
	if (ev->key < UI_KEY_COUNT) {
		if (ev->type == UI_EV_KEY_PRESS) {
			press_screen[ev->key] = s;
		} else if ((ev->type == UI_EV_KEY_LONG || ev->type == UI_EV_KEY_RELEASE) &&
			   press_screen[ev->key] != s) {
			return;
		}
	}
	if (s->event && s->event(s, ev)) {
		return;
	}
	if (ui_ev_back(ev)) {
		ui_pop();
	} else if (ui_ev_home(ev)) {
		ui_home();
	}
}

void ui_poll(void)
{
	struct ui_screen *s = ui_top();

	if (s && s->poll) {
		s->poll(s);
	}
}

void ui_invalidate(void)
{
	dirty = true;
}

bool ui_needs_render(void)
{
	return dirty;
}

void ui_render(struct gfx *g)
{
	struct ui_screen *s = ui_top();

	dirty = false;
	gfx_reset_clip(g);
	gfx_clear(g, GFX_WHITE);
	if (!s) {
		return;
	}
	if (!s->fullscreen && draw_statusbar) {
		gfx_set_clip(g, 0, 0, GFX_WIDTH, UI_STATUS_H);
		draw_statusbar(g);
		gfx_reset_clip(g);
		gfx_hline(g, 0, UI_STATUS_H, GFX_WIDTH, GFX_DARK);
		gfx_set_clip(g, 0, UI_CONTENT_Y, GFX_WIDTH, UI_CONTENT_H);
	}
	if (s->draw) {
		s->draw(s, g);
	}
	gfx_reset_clip(g);
}

int ui_draw_title(struct gfx *g, const char *title)
{
	gfx_fill_rect(g, 0, UI_CONTENT_Y, GFX_WIDTH, UI_TITLE_H, GFX_DARK);
	gfx_text_aligned(g, gfx_font_text, 2, UI_CONTENT_Y + 1, GFX_WIDTH - 4, GFX_ALIGN_CENTER,
			 ui_tr(title), GFX_WHITE);
	return UI_CONTENT_Y + UI_TITLE_H;
}

void ui_draw_scrollbar(struct gfx *g, int y, int h, int first, int visible, int total)
{
	if (total <= visible || total <= 0) {
		return;
	}
	int bar_h = h * visible / total;

	if (bar_h < 3) {
		bar_h = 3;
	}
	int bar_y = y + (h - bar_h) * first / (total - visible);

	gfx_vline(g, GFX_WIDTH - 1, y, h, GFX_LIGHT);
	gfx_fill_rect(g, GFX_WIDTH - 2, bar_y, 2, bar_h, GFX_BLACK);
}
