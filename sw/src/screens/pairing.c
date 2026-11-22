/*
 * Bluetooth pairing: the passkey a phone asks for, in large digits, while
 * the phone pairs. Closed when pairing ends; C cancels it.
 */

#include <stdio.h>

#include "../ble.h"
#include "../i18n.h"
#include "screens.h"

static bool pairing_event(struct ui_screen *s, const struct ui_event *ev)
{
	(void)s;
	if (ui_ev_back(ev)) {
		ble_pairing_cancel();
		ui_pop();
	}
	return true;
}

static void pairing_draw(struct ui_screen *s, struct gfx *g)
{
	int y = ui_draw_title(g, s->title) + 4;
	int key = ble_passkey();
	char buf[12];

	gfx_text_aligned(g, gfx_font_text, 0, y, GFX_WIDTH, GFX_ALIGN_CENTER, tr("Pairing PIN"),
			 GFX_BLACK);
	y += gfx_font_text->height + 4;
	if (key >= 0) {
		snprintf(buf, sizeof(buf), "%06d", key);
		gfx_text_aligned(g, gfx_font_large, 0, y, GFX_WIDTH, GFX_ALIGN_CENTER, buf,
				 GFX_BLACK);
	}
	y += gfx_font_large->height + 4;
	gfx_text_aligned(g, gfx_font_text, 0, y, GFX_WIDTH, GFX_ALIGN_CENTER,
			 tr("Enter it on the phone."), GFX_DARK);
}

static struct ui_screen pairing = {
	.title = "Bluetooth",
	.event = pairing_event,
	.draw = pairing_draw,
};

void pairing_show(void)
{
	if (ui_top() != &pairing) {
		ui_push(&pairing);
	}
	ui_invalidate();
}

void pairing_close(void)
{
	if (ui_top() == &pairing) {
		ui_pop();
	}
}
