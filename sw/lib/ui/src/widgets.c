#include <stdio.h>
#include <string.h>

#include "gfx/icons.h"
#include "ui/widgets.h"

/* Menu ------------------------------------------------------------------ */

static int menu_visible(void)
{
	return (GFX_HEIGHT - UI_CONTENT_Y - UI_TITLE_H) / UI_ROW_H;
}

static void menu_activate(struct ui_menu *m, int idx)
{
	const struct ui_menu_item *it = &m->items[idx];

	m->sel = idx;
	if (it->submenu) {
		ui_push(it->submenu);
	} else if (it->action) {
		it->action(m, idx);
	}
	ui_invalidate();
}

bool ui_menu_event(struct ui_screen *s, const struct ui_event *ev)
{
	struct ui_menu *m = CONTAINER_OF(s, struct ui_menu, screen);
	int n = m->count;

	if (n == 0) {
		return false;
	}

	if (ev->type == UI_EV_WHEEL) {
		m->sel = ((m->sel - ev->value) % n + n) % n;
		ui_invalidate();
		return true;
	}
	if (ev->type != UI_EV_KEY_PRESS) {
		return false;
	}

	int page = menu_visible();

	switch (ev->key) {
	case UI_KEY_SELECT:
	case UI_KEY_CALL:
		menu_activate(m, m->sel);
		return true;
	case UI_KEY_DOWN:
		m->sel = m->sel + page < n - 1 ? m->sel + page : n - 1;
		ui_invalidate();
		return true;
	default:
		if (ev->key >= UI_KEY_1 && ev->key <= UI_KEY_9 && ui_key_digit(ev->key) <= n) {
			menu_activate(m, ui_key_digit(ev->key) - 1);
			return true;
		}
		return false;
	}
}

void ui_menu_draw(struct ui_screen *s, struct gfx *g)
{
	struct ui_menu *m = CONTAINER_OF(s, struct ui_menu, screen);
	int y0 = ui_draw_title(g, s->title);
	int visible = menu_visible();
	bool scroll = m->count > visible;
	int right = GFX_WIDTH - (scroll ? 4 : 2);
	char buf[24];

	if (m->sel < m->top) {
		m->top = m->sel;
	} else if (m->sel >= m->top + visible) {
		m->top = m->sel - visible + 1;
	}

	for (int i = 0; i < visible && m->top + i < m->count; i++) {
		int idx = m->top + i;
		const struct ui_menu_item *it = &m->items[idx];
		int y = y0 + i * UI_ROW_H;
		bool sel = idx == m->sel;
		enum gfx_color fg = sel ? GFX_WHITE : GFX_BLACK;
		const char *val = m->value ? m->value(m, idx, buf, sizeof(buf)) : NULL;
		int val_w = 0;

		if (sel) {
			gfx_fill_rect(g, 0, y, right + 1, UI_ROW_H, GFX_BLACK);
		}
		if (val && *val) {
			val_w = gfx_text_width(gfx_font_text, val, -1) + 3;
			gfx_text(g, gfx_font_text, right - val_w + 3, y + 1, val,
				 sel ? GFX_LIGHT : GFX_DARK);
		} else if (it->submenu) {
			val_w = 5;
			gfx_draw_icon(g, right - 3, y + 2, &gfx_icon_arrow_r, fg);
		}
		gfx_text_fit(g, gfx_font_text, 2, y + 1, right - 2 - val_w, ui_tr(it->label), fg);
	}
	ui_draw_scrollbar(g, y0, visible * UI_ROW_H, m->top, visible, m->count);
}

/* Slider ---------------------------------------------------------------- */

void ui_slider_open(struct ui_slider *sl, int value)
{
	sl->value = value;
	sl->initial = value;
	ui_push(&sl->screen);
}

static void slider_set(struct ui_slider *sl, int v)
{
	if (v < sl->min) {
		v = sl->min;
	} else if (v > sl->max) {
		v = sl->max;
	}
	if (v != sl->value) {
		sl->value = v;
		if (sl->changed) {
			sl->changed(sl, v);
		}
		ui_invalidate();
	}
}

bool ui_slider_event(struct ui_screen *s, const struct ui_event *ev)
{
	struct ui_slider *sl = CONTAINER_OF(s, struct ui_slider, screen);

	if (ev->type == UI_EV_WHEEL) {
		slider_set(sl, sl->value + ev->value * sl->step);
		return true;
	}
	if (ui_ev_press(ev, UI_KEY_DOWN)) {
		int big = (sl->max - sl->min) / 10;

		if (big < sl->step) {
			big = sl->step;
		}
		slider_set(sl, sl->value - big);
		return true;
	}
	if (ui_ev_press(ev, UI_KEY_SELECT) || ui_ev_press(ev, UI_KEY_CALL)) {
		ui_pop();
		if (sl->done) {
			sl->done(sl, sl->value, true);
		}
		return true;
	}
	if (ui_ev_back(ev)) {
		slider_set(sl, sl->initial);
		ui_pop();
		if (sl->done) {
			sl->done(sl, sl->value, false);
		}
		return true;
	}
	return false;
}

void ui_slider_draw(struct ui_screen *s, struct gfx *g)
{
	struct ui_slider *sl = CONTAINER_OF(s, struct ui_slider, screen);
	int y = ui_draw_title(g, s->title);
	char buf[16];

	snprintf(buf, sizeof(buf), "%d%s", sl->value, sl->unit ? sl->unit : "");
	gfx_text_aligned(g, gfx_font_large, 0, y + 6, GFX_WIDTH, GFX_ALIGN_CENTER, buf,
			 GFX_BLACK);

	int bx = 8, bw = GFX_WIDTH - 16, by = y + 32, bh = 7;
	int range = sl->max - sl->min;
	int fill = range > 0 ? (bw - 4) * (sl->value - sl->min) / range : 0;

	gfx_rect(g, bx, by, bw, bh, GFX_BLACK);
	gfx_fill_rect(g, bx + 2, by + 2, fill, bh - 4, GFX_DARK);
	gfx_text_aligned(g, gfx_font_small, 0, by + bh + 2, GFX_WIDTH, GFX_ALIGN_CENTER,
			 ui_tr("push = ok   C = cancel"), GFX_DARK);
}

/* Text view ------------------------------------------------------------- */

#define TV_MARGIN 2

static int tv_visible(void)
{
	return (GFX_HEIGHT - UI_CONTENT_Y - UI_TITLE_H) / (gfx_font_text->height + 1);
}

static bool tv_event(struct ui_screen *s, const struct ui_event *ev)
{
	struct ui_textview *tv = CONTAINER_OF(s, struct ui_textview, screen);
	int max_scroll = tv->lines - tv_visible();

	if (max_scroll < 0) {
		max_scroll = 0;
	}
	if (ev->type == UI_EV_WHEEL || ui_ev_press(ev, UI_KEY_DOWN)) {
		int d = ev->type == UI_EV_WHEEL ? -ev->value : tv_visible();
		int ns = tv->scroll + d;

		tv->scroll = ns < 0 ? 0 : (ns > max_scroll ? max_scroll : ns);
		ui_invalidate();
		return true;
	}
	if (ui_ev_press(ev, UI_KEY_SELECT) || ui_ev_press(ev, UI_KEY_CALL)) {
		ui_pop();
		return true;
	}
	return false;
}

static void tv_draw(struct ui_screen *s, struct gfx *g)
{
	struct ui_textview *tv = CONTAINER_OF(s, struct ui_textview, screen);
	int y0 = ui_draw_title(g, s->title);
	int lh = gfx_font_text->height + 1;
	int visible = tv_visible();
	int width = GFX_WIDTH - 2 * TV_MARGIN - 2;
	const char *p = ui_tr(tv->text);
	int line = 0;
	char buf[64];

	while (*p) {
		const char *next;
		int len = gfx_wrap(gfx_font_text, p, width, &next);

		if (line >= tv->scroll && line < tv->scroll + visible) {
			if (len >= (int)sizeof(buf)) {
				len = sizeof(buf) - 1;
			}
			memcpy(buf, p, len);
			buf[len] = '\0';
			gfx_text(g, gfx_font_text, TV_MARGIN, y0 + 1 + (line - tv->scroll) * lh, buf,
				 GFX_BLACK);
		}
		line++;
		p = next;
	}
	tv->lines = line;
	ui_draw_scrollbar(g, y0, GFX_HEIGHT - y0, tv->scroll, visible, tv->lines);
}

void ui_textview_open(struct ui_textview *tv, const char *title, const char *text)
{
	tv->screen.title = title;
	tv->screen.event = tv_event;
	tv->screen.draw = tv_draw;
	tv->text = text;
	tv->scroll = 0;
	tv->lines = 0;
	ui_push(&tv->screen);
}

void ui_message(const char *title, const char *text)
{
	static struct ui_textview tv;

	ui_textview_open(&tv, title, text);
}

static struct ui_textview confirm_tv;
static void (*confirm_yes)(void);

static bool confirm_event(struct ui_screen *s, const struct ui_event *ev)
{
	if (ui_ev_press(ev, UI_KEY_SELECT) || ui_ev_press(ev, UI_KEY_CALL)) {
		void (*yes)(void) = confirm_yes;

		ui_pop();
		if (yes) {
			yes();
		}
		return true;
	}
	/* Scrolling as in the text view; C and bend up go back (cancel). */
	return ev->type == UI_EV_WHEEL || ui_ev_press(ev, UI_KEY_DOWN) ? tv_event(s, ev) : false;
}

void ui_confirm(const char *title, const char *text, void (*yes)(void))
{
	confirm_yes = yes;
	ui_textview_open(&confirm_tv, title, text);
	confirm_tv.screen.event = confirm_event;
}
