/*
 * One conversation: the newest messages at the bottom, each with a small
 * header (sender and time, for own messages the delivery status). The wheel
 * scrolls; push or CALL writes a reply.
 */

#include <stdio.h>

#include <zephyr/sys/util.h>
#include <string.h>

#include "../clock.h"
#include "../i18n.h"
#include "../mesh/mesh.h"
#include "../mesh/msgstore.h"
#include "meshtastic/packet.h"
#include "screens.h"

/* Messages shown (copied from the store on each redraw). */
#define SHOWN     12
#define LINE_H    8
#define HEAD_H    7
#define MAX_ROWS  96

static uint32_t conv;
static int scroll; /* rows scrolled up from the bottom */

struct ui_screen chat_screen;

static struct msg shown[SHOWN];
static int nshown;

/* A row is a message header or one wrapped line of a message. */
struct row {
	uint8_t msg;
	uint8_t head;     /* 1 = header row */
	uint16_t offset;  /* start of the line in the text */
	uint8_t len;
};

static struct row rows[MAX_ROWS];
static int nrows;

void chat_open(uint32_t c)
{
	conv = c;
	scroll = 0;
	ui_push(&chat_screen);
}

bool chat_showing(uint32_t c)
{
	return ui_top() == &chat_screen && conv == c;
}

static void load(void)
{
	int idx[SHOWN];
	int n = 0;

	msgstore_lock();
	for (int i = msgstore_count() - 1; i >= 0 && n < SHOWN; i--) {
		if (msgstore_get(i)->conv == conv) {
			idx[n++] = i;
		}
	}
	nshown = n;
	for (int i = 0; i < n; i++) {
		shown[n - 1 - i] = *msgstore_get(idx[i]); /* oldest first */
	}
	msgstore_mark_read(conv);
	msgstore_unlock();

	nrows = 0;
	for (int m = 0; m < nshown && nrows < MAX_ROWS; m++) {
		const char *t = shown[m].text;
		const char *p = t;
		int width = GFX_WIDTH - 3 - (shown[m].outgoing ? 8 : 0);

		rows[nrows++] = (struct row){.msg = m, .head = 1};
		while (*p && nrows < MAX_ROWS) {
			const char *next;
			int len = gfx_wrap(gfx_font_text, p, width, &next);

			rows[nrows++] = (struct row){.msg = m, .offset = p - t, .len = len};
			p = next;
		}
	}
}

static int row_h(const struct row *r)
{
	return r->head ? HEAD_H : LINE_H;
}

static const char *status_text(const struct msg *m)
{
	switch (m->status) {
	case MSG_QUEUED:
		return tr("sending");
	case MSG_SENT:
		return tr("sent");
	case MSG_DELIVERED:
		return tr("delivered");
	case MSG_NO_ACK:
		return tr("no relay");
	case MSG_FAILED:
		return tr("failed");
	default:
		return "";
	}
}

static void draw_header(struct gfx *g, int y, const struct msg *m)
{
	char who[20], line[40], when[8] = "";

	if (m->time) {
		struct clock_tm tm;

		clock_local(m->time, &tm);
		snprintf(when, sizeof(when), "%02d:%02d", tm.hour, tm.min);
	}
	if (m->outgoing) {
		snprintf(line, sizeof(line), "%s %s", when, status_text(m));
		gfx_text_aligned(g, gfx_font_small, 0, y, GFX_WIDTH - 2, GFX_ALIGN_RIGHT, line,
				 m->status == MSG_FAILED ? GFX_BLACK : GFX_DARK);
	} else {
		mesh_node_name(m->from, who, sizeof(who));
		snprintf(line, sizeof(line), "%s %s", who, when);
		gfx_text_fit(g, gfx_font_small, 1, y, GFX_WIDTH - 3, line, GFX_DARK);
	}
}

static void chat_draw(struct ui_screen *s, struct gfx *g)
{
	char title[24];
	char buf[MSG_TEXT_MAX + 1];

	ARG_UNUSED(s);
	if (conv == CONV_CHANNEL) {
		snprintf(title, sizeof(title), "#%s", mesh_channel_name());
	} else {
		mesh_node_name(conv, title, sizeof(title));
	}
	int y0 = ui_draw_title(g, title);
	int area = GFX_HEIGHT - y0;

	load();
	if (nrows == 0) {
		gfx_text_aligned(g, gfx_font_text, 0, y0 + 20, GFX_WIDTH, GFX_ALIGN_CENTER,
				 tr("No messages"), GFX_DARK);
		gfx_text_aligned(g, gfx_font_small, 0, y0 + 32, GFX_WIDTH, GFX_ALIGN_CENTER,
				 tr("push: write"), GFX_DARK);
		return;
	}
	if (scroll > nrows - 1) {
		scroll = nrows - 1;
	}
	/* Fill from the bottom upwards, starting `scroll` rows above the last. */
	int y = GFX_HEIGHT;
	int first = nrows - 1 - scroll;

	gfx_set_clip(g, 0, y0, GFX_WIDTH, area);
	for (int i = first; i >= 0 && y > y0; i--) {
		const struct row *r = &rows[i];
		const struct msg *m = &shown[r->msg];

		y -= row_h(r);
		if (r->head) {
			draw_header(g, y, m);
		} else {
			int x = m->outgoing ? 9 : 1;

			memcpy(buf, m->text + r->offset, r->len);
			buf[r->len] = '\0';
			gfx_text(g, gfx_font_text, x, y, buf, GFX_BLACK);
			if (m->outgoing) {
				gfx_vline(g, 6, y, LINE_H, GFX_LIGHT);
			}
		}
	}
	gfx_reset_clip(g);
	ui_draw_scrollbar(g, y0, area, nrows - 1 - first, area / LINE_H, nrows);
}

static bool chat_event(struct ui_screen *s, const struct ui_event *ev)
{
	ARG_UNUSED(s);
	if (ev->type == UI_EV_WHEEL) {
		scroll += ev->value; /* up = older */
		if (scroll < 0) {
			scroll = 0;
		}
		ui_invalidate();
		return true;
	}
	if (ui_ev_press(ev, UI_KEY_SELECT) || ui_ev_press(ev, UI_KEY_CALL)) {
		compose_open_message(conv == CONV_CHANNEL ? MT_BROADCAST : conv);
		return true;
	}
	if (ui_ev_press(ev, UI_KEY_MEMO) && conv != CONV_CHANNEL) {
		node_detail_open(conv);
		return true;
	}
	return false;
}

static void chat_enter(struct ui_screen *s)
{
	ARG_UNUSED(s);
	scroll = 0;
}

struct ui_screen chat_screen = {
	.event = chat_event,
	.draw = chat_draw,
	.enter = chat_enter,
};
