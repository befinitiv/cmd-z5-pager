/*
 * Compose a message with predictive text.
 *
 * Layout: mode and length on the first line, the wrapped text with the word
 * being composed (the completion part in gray, underlined) and a cursor, and
 * the candidate bar at the bottom. Memo opens the symbol picker. The text is
 * kept as a draft when leaving the screen.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "../i18n.h"
#include "../textin.h"
#include "screens.h"

#define HEADER_Y  UI_CONTENT_Y
#define TEXT_Y    (UI_CONTENT_Y + 7)
#define BAR_H     9
#define BAR_Y     (GFX_HEIGHT - BAR_H)
#define LINE_H    8
#define TEXT_W    (GFX_WIDTH - 2)
#define MAX_LINES 16

static const enum ti_ekey digit_keys[] = {
	[UI_KEY_0] = TI_EK_0,     [UI_KEY_1] = TI_EK_0 + 1, [UI_KEY_2] = TI_EK_0 + 2,
	[UI_KEY_3] = TI_EK_0 + 3, [UI_KEY_4] = TI_EK_0 + 4, [UI_KEY_5] = TI_EK_0 + 5,
	[UI_KEY_6] = TI_EK_0 + 6, [UI_KEY_7] = TI_EK_0 + 7, [UI_KEY_8] = TI_EK_0 + 8,
	[UI_KEY_9] = TI_EK_0 + 9,
};

static bool editor_key(enum ui_key k, enum ti_ekey *out)
{
	if (ui_key_is_digit(k)) {
		*out = digit_keys[k];
	} else if (k == UI_KEY_STAR) {
		*out = TI_EK_STAR;
	} else if (k == UI_KEY_POUND) {
		*out = TI_EK_POUND;
	} else if (k == UI_KEY_CLEAR) {
		*out = TI_EK_CLEAR;
	} else {
		return false;
	}
	return true;
}

static bool compose_event(struct ui_screen *s, const struct ui_event *ev)
{
	struct ti_editor *ed = textin_editor();
	uint32_t now = k_uptime_get_32();
	enum ti_ekey key;
	bool used = false;

	ARG_UNUSED(s);

	switch (ev->type) {
	case UI_EV_KEY_RELEASE:
		/* Keys act on release, so a long press can mean something else. */
		if (ev->value == 0 && editor_key(ev->key, &key)) {
			used = ti_editor_key(ed, key, false, now);
		} else if (ev->value && editor_key(ev->key, &key)) {
			used = true; /* the long press was handled already */
		}
		break;
	case UI_EV_KEY_LONG:
		if (editor_key(ev->key, &key)) {
			used = ti_editor_key(ed, key, true, now);
		}
		break;
	case UI_EV_KEY_PRESS:
		if (ev->key == UI_KEY_MEMO) {
			ti_editor_symbols(ed, !ed->symbols);
			used = true;
		} else if (ev->key == UI_KEY_SELECT) {
			used = ti_editor_select(ed);
		} else if (ev->key == UI_KEY_CALL) {
			ui_message("New message", "Sending messages follows in phase 3.");
			used = true;
		} else if (editor_key(ev->key, &key)) {
			used = true; /* handled on release */
		}
		break;
	case UI_EV_WHEEL:
		used = ti_editor_wheel(ed, ev->value);
		break;
	default:
		break;
	}
	if (used) {
		ui_invalidate();
	}
	return used;
}

static void compose_poll(struct ui_screen *s)
{
	ARG_UNUSED(s);
	if (ti_editor_tick(textin_editor(), k_uptime_get_32())) {
		ui_invalidate();
	}
}

static void compose_leave(struct ui_screen *s)
{
	ARG_UNUSED(s);
	/* Store the learned words (not after every word, to spare the flash). */
	textin_save();
}

/* Draw the text with the composing word; returns nothing, scrolls to the end. */
static void draw_text(struct gfx *g, struct ti_editor *ed)
{
	static char buf[TI_TEXT_MAX + TI_WORD_BYTES + 1];
	int typed;
	const char *comp = ti_editor_composing(ed, &typed);
	int text_len = strlen(ti_editor_text(ed));
	int comp_typed_end = text_len;
	const char *p;

	snprintf(buf, sizeof(buf), "%s%s", ti_editor_text(ed), comp);
	/* Byte offset where the typed part of the composing word ends. */
	p = comp;
	for (int i = 0; i < typed && *p; i++) {
		ti_utf8_next(&p);
	}
	comp_typed_end += p - comp;
	int total = strlen(buf);

	/* Wrap into lines (start offsets). */
	int starts[MAX_LINES + 1];
	int nlines = 0;
	const char *line = buf;

	do {
		const char *next;

		gfx_wrap(gfx_font_text, line, TEXT_W - 2, &next);
		if (nlines == MAX_LINES) {
			memmove(starts, starts + 1, (MAX_LINES - 1) * sizeof(int));
			nlines--;
		}
		starts[nlines++] = line - buf;
		line = next;
	} while (*line);
	/* A text ending in a space continues on the same line. */
	starts[nlines] = total;

	int visible = (BAR_Y - TEXT_Y) / LINE_H;
	int first = nlines > visible ? nlines - visible : 0;
	int cx = 1, cy = TEXT_Y;

	for (int l = first; l < nlines; l++) {
		int y = TEXT_Y + (l - first) * LINE_H;
		int x = 1;
		const char *q = buf + starts[l];
		const char *end = buf + starts[l + 1];

		while (q < end && *q) {
			int off = q - buf;
			uint32_t cp = gfx_utf8_next(&q);
			enum gfx_color c = off >= comp_typed_end ? GFX_DARK : GFX_BLACK;
			int x0 = x;

			if (cp == '\n') {
				continue;
			}
			x = gfx_char(g, gfx_font_text, x, y, cp, c);
			if (off >= text_len) {
				gfx_hline(g, x0, y + 7, x - x0, GFX_DARK); /* underline the word */
			}
		}
		cx = x;
		cy = y;
	}
	/* Cursor; a blinking one would need a timer, a solid bar is clear enough. */
	gfx_vline(g, cx, cy, 7, GFX_BLACK);
	ui_draw_scrollbar(g, TEXT_Y, BAR_Y - TEXT_Y, first, visible, nlines);
}

static void draw_bar(struct gfx *g, struct ti_editor *ed)
{
	const char *items[TI_EDIT_CANDS > 40 ? TI_EDIT_CANDS : 40];
	int n, sel;

	gfx_fill_rect(g, 0, BAR_Y, GFX_WIDTH, BAR_H, GFX_LIGHT);
	if (ed->symbols) {
		n = ti_editor_symbol_count();
		for (int i = 0; i < n; i++) {
			items[i] = ti_editor_symbol(i);
		}
		sel = ed->sym_sel;
	} else {
		n = ed->ncands;
		for (int i = 0; i < n; i++) {
			items[i] = ed->cand_text[i];
		}
		sel = ed->sel;
	}
	if (n == 0) {
		gfx_text_aligned(g, gfx_font_small, 0, BAR_Y + 2, GFX_WIDTH, GFX_ALIGN_CENTER,
				 tr("CALL = send"), GFX_DARK);
		return;
	}

	/* Scroll so that the selected entry is visible. */
	int widths[40];
	int x = 1, start = 0;

	for (int i = 0; i < n; i++) {
		widths[i] = gfx_text_width(gfx_font_text, items[i], -1) + 4;
	}
	if (sel > 0) {
		int w = 0;

		for (int i = sel; i >= 0; i--) {
			w += widths[i];
			if (w > GFX_WIDTH - 2) {
				start = i + 1;
				break;
			}
		}
	}
	for (int i = start; i < n && x < GFX_WIDTH; i++) {
		bool on = i == sel;

		if (on) {
			gfx_fill_rect(g, x - 1, BAR_Y + 1, widths[i] - 1, BAR_H - 1, GFX_BLACK);
		}
		gfx_text(g, gfx_font_text, x + 1, BAR_Y + 1, items[i],
			 on ? GFX_WHITE : (sel < 0 ? GFX_DARK : GFX_BLACK));
		x += widths[i];
	}
}

static void compose_draw(struct ui_screen *s, struct gfx *g)
{
	struct ti_editor *ed = textin_editor();
	char buf[24];

	ARG_UNUSED(s);
	snprintf(buf, sizeof(buf), "%s %s", ti_editor_mode_label(ed),
		 textin_lang() == LANG_DE ? "DE" : "EN");
	gfx_text(g, gfx_font_small, 1, HEADER_Y, buf, GFX_DARK);
	snprintf(buf, sizeof(buf), "%u/%u", (unsigned)strlen(ti_editor_text(ed)), TI_TEXT_MAX);
	gfx_text_aligned(g, gfx_font_small, 0, HEADER_Y, GFX_WIDTH - 1, GFX_ALIGN_RIGHT, buf,
			 GFX_DARK);
	draw_text(g, ed);
	draw_bar(g, ed);
}

struct ui_screen compose_screen = {
	.title = "New message",
	.leave = compose_leave,
	.event = compose_event,
	.draw = compose_draw,
	.poll = compose_poll,
};
