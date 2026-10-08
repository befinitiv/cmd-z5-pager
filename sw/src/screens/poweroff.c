/*
 * Shown while the power key is held long: "release to switch off", then a
 * goodbye message while the device shuts down.
 */

#include <string.h>

#include "../i18n.h"
#include "screens.h"

static const char *message;

static bool poweroff_event(struct ui_screen *s, const struct ui_event *ev)
{
	(void)s;
	(void)ev;
	/* Nothing else can be done while the key is held. */
	return true;
}

static void poweroff_draw(struct ui_screen *s, struct gfx *g)
{
	int y = ui_draw_title(g, s->title) + 12;
	const char *p = tr(message);
	char line[40];

	while (*p) {
		const char *next;
		int len = gfx_wrap(gfx_font_text, p, GFX_WIDTH - 8, &next);

		if (len >= (int)sizeof(line)) {
			len = sizeof(line) - 1;
		}
		memcpy(line, p, len);
		line[len] = '\0';
		gfx_text_aligned(g, gfx_font_text, 0, y, GFX_WIDTH, GFX_ALIGN_CENTER, line, GFX_BLACK);
		y += gfx_font_text->height + 2;
		p = next;
	}
}

static struct ui_screen poweroff = {
	.title = "Power off",
	.event = poweroff_event,
	.draw = poweroff_draw,
};

void poweroff_show(void)
{
	message = "Release the key to switch off.";
	if (ui_top() != &poweroff) {
		ui_push(&poweroff);
	}
	ui_invalidate();
}

void poweroff_goodbye(void)
{
	message = "Goodbye";
	ui_invalidate();
}
