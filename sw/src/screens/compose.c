/*
 * Compose a message with predictive text.
 *
 * Layout: mode and length on the first line, the wrapped text with the word
 * being composed (the completion part in gray, underlined) and a cursor, and
 * the candidate bar at the bottom (while key 1 is pending: its punctuation
 * choices). Memo opens the symbol picker; '*' switches the wheel between the
 * candidates and moving the (then blinking) cursor.
 * The text is kept as a draft when leaving the screen.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "../i18n.h"
#include "../mesh/mesh.h"
#include "../textin.h"
#include "meshtastic/packet.h"
#include "screens.h"

#define HEADER_Y  UI_CONTENT_Y
#define TEXT_Y    (UI_CONTENT_Y + 7)
#define BAR_H     9
#define BAR_Y     (GFX_HEIGHT - BAR_H)
#define LINE_H    8
#define TEXT_W    (GFX_WIDTH - 2)
#define MAX_LINES 40
#define BLINK_MS  400

static const enum ti_ekey digit_keys[] = {
	[UI_KEY_0] = TI_EK_0,     [UI_KEY_1] = TI_EK_0 + 1, [UI_KEY_2] = TI_EK_0 + 2,
	[UI_KEY_3] = TI_EK_0 + 3, [UI_KEY_4] = TI_EK_0 + 4, [UI_KEY_5] = TI_EK_0 + 5,
	[UI_KEY_6] = TI_EK_0 + 6, [UI_KEY_7] = TI_EK_0 + 7, [UI_KEY_8] = TI_EK_0 + 8,
	[UI_KEY_9] = TI_EK_0 + 9,
};

/* What the text is for: a message, or editing a setting (e.g. the node name). */
static struct {
	bool edit;
	uint32_t to;
	char title[24];
	size_t max_len;
	void (*done)(const char *text);
	/* The message draft, kept while a setting is edited. */
	char draft[TI_TEXT_MAX + 1];
} ctx = {.to = MT_BROADCAST};

static bool blink_on = true;

void compose_open_message(uint32_t to)
{
	ctx.edit = false;
	ctx.to = to;
	ui_push(&compose_screen);
}

void compose_open_edit(const char *title, const char *text, size_t max_len,
		       void (*done)(const char *text))
{
	struct ti_editor *ed = textin_editor();

	if (!ctx.edit) {
		strcpy(ctx.draft, ti_editor_text(ed));
	}
	ctx.edit = true;
	snprintf(ctx.title, sizeof(ctx.title), "%s", title);
	ctx.max_len = max_len;
	ctx.done = done;
	ti_editor_set_text(ed, text);
	ui_push(&compose_screen);
}

/* Text without trailing spaces, cut to max bytes at a character boundary. */
static void final_text(struct ti_editor *ed, char *out, size_t max)
{
	const char *t = ti_editor_text(ed);
	size_t n = strlen(t);

	while (n && t[n - 1] == ' ') {
		n--;
	}
	if (n > max) {
		n = max;
		while (n && ((uint8_t)t[n] & 0xc0) == 0x80) {
			n--;
		}
	}
	memcpy(out, t, n);
	out[n] = '\0';
}

/* CALL: send the message, or save the edited setting. */
static void finish(void)
{
	struct ti_editor *ed = textin_editor();
	char text[TI_TEXT_MAX + 1];

	ti_editor_commit(ed);
	final_text(ed, text, ctx.edit ? ctx.max_len : TI_TEXT_MAX);
	if (ctx.edit) {
		void (*done)(const char *) = ctx.done;

		ui_pop(); /* restores the draft */
		if (done) {
			done(text);
		}
		return;
	}
	if (!text[0]) {
		return;
	}
	if (mesh_send_text(ctx.to, text) != 0) {
		ui_message("New message", "The radio is not available.");
		return;
	}
	ti_editor_clear(ed);
	ui_pop();
}
static uint32_t blink_time;

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
			finish();
			used = true;
		} else if (editor_key(ev->key, &key)) {
			used = true; /* handled on release */
		}
		break;
	case UI_EV_WHEEL:
		used = ti_editor_wheel(ed, ev->value, now);
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
	struct ti_editor *ed = textin_editor();
	uint32_t now = k_uptime_get_32();

	ARG_UNUSED(s);
	if (ti_editor_tick(ed, now)) {
		ui_invalidate();
	}
	if (ed->cursor_mode && now - blink_time >= BLINK_MS) {
		blink_on = !blink_on;
		blink_time = now;
		ui_invalidate();
	} else if (!ed->cursor_mode) {
		blink_on = true;
	}
}

static void compose_leave(struct ui_screen *s)
{
	ARG_UNUSED(s);
	/* Store the learned words (not after every word, to spare the flash). */
	textin_save();
	if (ctx.edit) {
		/* Saved or cancelled: back to the message draft. */
		ctx.edit = false;
		ti_editor_set_text(textin_editor(), ctx.draft);
	}
}

/* Draw the text with the composing word at the cursor; scrolls to the cursor. */
static void draw_text(struct gfx *g, struct ti_editor *ed)
{
	static char buf[TI_TEXT_MAX + TI_WORD_BYTES + 1];
	static int first; /* first visible line, kept between redraws */
	const char *text = ti_editor_text(ed);
	int cursor = ti_editor_cursor(ed);
	int typed;
	const char *comp = ti_editor_composing(ed, &typed);
	int comp_len = strlen(comp);
	const char *p = comp;

	/* text before the cursor + composing word + text after the cursor */
	snprintf(buf, sizeof(buf), "%.*s%s%s", cursor, text, comp, text + cursor);
	for (int i = 0; i < typed && *p; i++) {
		ti_utf8_next(&p);
	}
	int comp_typed_end = cursor + (p - comp);
	int comp_end = cursor + comp_len;
	int total = strlen(buf);

	/* Wrap into lines (start offsets); find the cursor's line. */
	int starts[MAX_LINES + 1];
	int nlines = 0, cursor_line = 0;
	const char *line = buf;

	do {
		const char *next;

		gfx_wrap(gfx_font_text, line, TEXT_W - 2, &next);
		starts[nlines] = line - buf;
		if (comp_end >= starts[nlines]) {
			cursor_line = nlines;
		}
		nlines++;
		line = next;
	} while (*line && nlines < MAX_LINES);
	starts[nlines] = total;

	int visible = (BAR_Y - TEXT_Y) / LINE_H;

	if (cursor_line < first) {
		first = cursor_line;
	} else if (cursor_line >= first + visible) {
		first = cursor_line - visible + 1;
	}
	if (first > nlines - visible) {
		first = nlines > visible ? nlines - visible : 0;
	}

	int cx = 1, cy = TEXT_Y;

	for (int l = first; l < nlines && l < first + visible; l++) {
		int y = TEXT_Y + (l - first) * LINE_H;
		int x = 1;
		const char *q = buf + starts[l];
		const char *end = buf + starts[l + 1];

		if (l == cursor_line && comp_end == starts[l]) {
			cx = x;
			cy = y;
		}
		while (q < end && *q) {
			int off = q - buf;
			uint32_t cp = gfx_utf8_next(&q);
			bool in_comp = off >= cursor && off < comp_end;
			int x0 = x;

			x = gfx_char(g, gfx_font_text, x, y, cp,
				     in_comp && off >= comp_typed_end ? GFX_DARK : GFX_BLACK);
			if (in_comp) {
				gfx_hline(g, x0, y + 7, x - x0, GFX_DARK); /* underline the word */
			}
			if (q - buf == comp_end && l == cursor_line) {
				cx = x;
				cy = y;
			}
		}
	}
	/*
	 * Solid cursor; it blinks while the wheel moves it. Drawn in the spacing
	 * column of the previous glyph, so it does not cover the next letter.
	 */
	if (!ed->cursor_mode || blink_on) {
		gfx_vline(g, cx > 1 ? cx - 1 : cx, cy, 7, GFX_BLACK);
	}
	ui_draw_scrollbar(g, TEXT_Y, BAR_Y - TEXT_Y, first, visible, nlines);
}

static void draw_bar(struct gfx *g, struct ti_editor *ed)
{
	const char *items[TI_EDIT_CANDS > 40 ? TI_EDIT_CANDS : 40];
	static char pending[40][5];
	int n, sel;

	gfx_fill_rect(g, 0, BAR_Y, GFX_WIDTH, BAR_H, GFX_LIGHT);
	if (ed->cursor_mode) {
		gfx_text_aligned(g, gfx_font_small, 0, BAR_Y + 2, GFX_WIDTH, GFX_ALIGN_CENTER,
				 tr("wheel: move  *: done"), GFX_BLACK);
		return;
	}
	n = ti_editor_pending_symbols(ed, pending, ARRAY_SIZE(pending), &sel);
	if (n) {
		/* Key 1: the punctuation choices, the pending one marked. */
		for (int i = 0; i < n; i++) {
			items[i] = pending[i];
		}
	} else if (ed->symbols) {
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
				 tr(ctx.edit ? "CALL = save" : "CALL = send"), GFX_DARK);
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
	if (ctx.edit) {
		snprintf(buf, sizeof(buf), "%s", tr(ctx.title));
	} else if (ctx.to != MT_BROADCAST) {
		char name[16];

		snprintf(buf, sizeof(buf), "> %s", mesh_node_name(ctx.to, name, sizeof(name)));
	} else {
		snprintf(buf, sizeof(buf), "%u/%u", (unsigned)strlen(ti_editor_text(ed)), TI_TEXT_MAX);
	}
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
