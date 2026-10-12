/*
 * About: git commit and version, Zephyr version and build date; push (or
 * the wheel) shows a QR code with the project's address.
 */

#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/version.h>

#include "../i18n.h"
#include "git_version.h"
#include "qr_project.h"
#include "screens.h"

#define LINE_H    8
#define QR_MODULE 2 /* px per module: 58 px, with a quiet zone in 72 px */

static bool show_qr;

static void draw_qr(struct gfx *g)
{
	int size = QR_PROJECT_SIZE * QR_MODULE;
	int x0 = (GFX_WIDTH - size) / 2, y0 = (GFX_HEIGHT - size) / 2;

	for (int y = 0; y < QR_PROJECT_SIZE; y++) {
		for (int x = 0; x < QR_PROJECT_SIZE; x++) {
			if (qr_project[y][x / 8] & (0x80 >> (x % 8))) {
				gfx_fill_rect(g, x0 + x * QR_MODULE, y0 + y * QR_MODULE, QR_MODULE,
					      QR_MODULE, GFX_BLACK);
			}
		}
	}
}

static void about_draw(struct ui_screen *s, struct gfx *g)
{
	char lines[4][32];
	int y;

	if (show_qr) {
		draw_qr(g);
		return;
	}
	y = ui_draw_title(g, s->title);
	/* "+" marks a build with uncommitted changes. */
	snprintf(lines[0], sizeof(lines[0]), "%s %s%s", tr("Commit"), GIT_HASH,
		 GIT_DIRTY ? "+" : "");
	snprintf(lines[1], sizeof(lines[1]), "%s", GIT_DESCRIBE);
	snprintf(lines[2], sizeof(lines[2]), "Zephyr %s", KERNEL_VERSION_STRING);
	snprintf(lines[3], sizeof(lines[3]), "%s %s", tr("Built"), __DATE__);
	for (int i = 0; i < ARRAY_SIZE(lines); i++) {
		/* The git version can be long: small font. */
		gfx_text_fit(g, i == 1 ? gfx_font_small : gfx_font_text, 2, y + 1 + i * LINE_H + (i == 1),
			     GFX_WIDTH - 4, lines[i], i == 1 ? GFX_DARK : GFX_BLACK);
	}
	gfx_fill_rect(g, 0, GFX_HEIGHT - 8, GFX_WIDTH, 8, GFX_LIGHT);
	gfx_text_aligned(g, gfx_font_small, 0, GFX_HEIGHT - 7, GFX_WIDTH, GFX_ALIGN_CENTER,
			 tr("push: QR code"), GFX_BLACK);
}

static bool about_event(struct ui_screen *s, const struct ui_event *ev)
{
	ARG_UNUSED(s);
	if (ui_ev_press(ev, UI_KEY_SELECT) || ev->type == UI_EV_WHEEL) {
		show_qr = !show_qr;
		ui_invalidate();
		return true;
	}
	/* Back from the QR code to the text first. */
	if (show_qr && ui_ev_back(ev)) {
		show_qr = false;
		ui_invalidate();
		return true;
	}
	return false;
}

static struct ui_screen about_screen = {
	.title = "About",
	.event = about_event,
	.draw = about_draw,
};

void about_show(void)
{
	show_qr = false;
	ui_push(&about_screen);
}
