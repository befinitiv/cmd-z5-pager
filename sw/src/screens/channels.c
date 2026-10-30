/*
 * Private channels (Settings > Mesh > Channels): three slots next to the
 * public primary channel. A channel has a name and either a number key
 * (Meshtastic's 1-byte keys: set the same number in the app, separates
 * groups but is not secret) or a passphrase key, which the Meshtastic app
 * gets from the channel's QR code. See mesh.h.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <qrcodegen.h>

#include "../i18n.h"
#include "../mesh/mesh.h"
#include "screens.h"

/* Passphrases shorter than this are refused. */
#define MIN_PASSPHRASE 8
/* Large enough for the longest channel link (version 5 at low error correction). */
#define QR_MAX_VERSION 8

static int slot; /* the channel being edited */
static char title[MESH_CHANNEL_NAME + 2];

static struct ui_menu channel_menu;

/* QR code ------------------------------------------------------------------- */

static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
static bool qr_ok;

static void qr_draw(struct ui_screen *s, struct gfx *g)
{
	ARG_UNUSED(s);
	gfx_fill_rect(g, 0, 0, GFX_WIDTH, GFX_HEIGHT, GFX_WHITE);
	if (!qr_ok) {
		gfx_text_aligned(g, gfx_font_text, 0, 30, GFX_WIDTH, GFX_ALIGN_CENTER,
				 tr("No QR code"), GFX_BLACK);
		return;
	}
	int n = qrcodegen_getSize(qr);
	/*
	 * Two pixels per module if it fits with a module of margin (the screen is
	 * white, so the sides have more), else one.
	 */
	int px = (n + 2) * 2 <= GFX_HEIGHT ? 2 : 1;
	int x0 = (GFX_WIDTH - n * px) / 2, y0 = (GFX_HEIGHT - n * px) / 2;

	for (int y = 0; y < n; y++) {
		for (int x = 0; x < n; x++) {
			if (qrcodegen_getModule(qr, x, y)) {
				gfx_fill_rect(g, x0 + x * px, y0 + y * px, px, px, GFX_BLACK);
			}
		}
	}
}

static bool qr_event(struct ui_screen *s, const struct ui_event *ev)
{
	ARG_UNUSED(s);
	/* Any key closes it (C and bend up by the default handling). */
	if (ui_ev_press(ev, UI_KEY_SELECT) || ui_ev_press(ev, UI_KEY_CALL)) {
		ui_pop();
		return true;
	}
	return ev->type == UI_EV_WHEEL;
}

static struct ui_screen qr_screen = {
	.title = "QR code",
	.event = qr_event,
	.draw = qr_draw,
};

static void show_qr(void)
{
	static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
	char url[128];

	qr_ok = mesh_channel_url(slot, url, sizeof(url)) > 0 &&
		qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
				     QR_MAX_VERSION, qrcodegen_Mask_AUTO, true);
	ui_push(&qr_screen);
}

/* One channel --------------------------------------------------------------- */

static void set_title(void)
{
	struct mesh_channel c;

	if (mesh_get_channel(slot, &c)) {
		snprintf(title, sizeof(title), "#%s", c.name);
	}
	channel_menu.screen.title = title;
}

static void rename_done(const char *text)
{
	struct mesh_channel c;

	if (!text[0] || !mesh_get_channel(slot, &c)) {
		return;
	}
	snprintf(c.name, sizeof(c.name), "%s", text);
	mesh_set_channel(slot, &c);
	set_title();
}

static void number_done(struct ui_slider *s, int v, bool accepted)
{
	struct mesh_channel c;

	ARG_UNUSED(s);
	if (accepted && mesh_get_channel(slot, &c)) {
		c.key_type = MESH_KEY_NUMBER;
		c.number = v;
		mesh_set_channel(slot, &c);
	}
}

static struct ui_slider number_slider = UI_SLIDER("Number", 1, 255, 1, NULL, NULL, number_done);

static void passphrase_done(const char *text)
{
	struct mesh_channel c;

	if (!text[0] || !mesh_get_channel(slot, &c)) {
		return;
	}
	if (strlen(text) < MIN_PASSPHRASE) {
		ui_message("Passphrase", "Too short: at least 8 characters.");
		return;
	}
	c.key_type = MESH_KEY_PASSPHRASE;
	mesh_key_from_passphrase(text, c.key);
	mesh_set_channel(slot, &c);
	/* The app needs the key: offer it right away. */
	show_qr();
}

static void delete_yes(void)
{
	mesh_set_channel(slot, NULL);
	ui_pop(); /* back to the channel list */
}

static void channel_action(struct ui_menu *m, int idx)
{
	struct mesh_channel c;

	ARG_UNUSED(m);
	if (!mesh_get_channel(slot, &c)) {
		return;
	}
	switch (idx) {
	case 0:
		compose_open_edit("Channel name", c.name, MESH_CHANNEL_NAME, rename_done);
		break;
	case 1:
		ui_slider_open(&number_slider, c.key_type == MESH_KEY_NUMBER ? c.number : 2);
		break;
	case 2:
		compose_open_edit("Passphrase", "", 64, passphrase_done);
		break;
	case 3:
		show_qr();
		break;
	case 4:
		ui_confirm("Delete channel", "Delete the channel and its messages?", delete_yes);
		break;
	default:
		break;
	}
}

static const char *channel_value(struct ui_menu *m, int idx, char *buf, size_t len)
{
	struct mesh_channel c;

	ARG_UNUSED(m);
	if (!mesh_get_channel(slot, &c)) {
		return NULL;
	}
	switch (idx) {
	case 0:
		snprintf(buf, len, "%s", c.name);
		return buf;
	case 1:
		if (c.key_type != MESH_KEY_NUMBER) {
			return "-";
		}
		snprintf(buf, len, "%u", c.number);
		return buf;
	case 2:
		return c.key_type == MESH_KEY_PASSPHRASE ? tr("set") : "-";
	default:
		return NULL;
	}
}

static const struct ui_menu_item channel_items[] = {
	{"Name", NULL, channel_action},
	{"Number", NULL, channel_action},
	{"Passphrase", NULL, channel_action},
	{"QR code", NULL, channel_action},
	{"Delete", NULL, channel_action},
};

static struct ui_menu channel_menu = UI_MENU("Channel", channel_items, channel_value);

/* Channel list -------------------------------------------------------------- */

static void open_channel(void)
{
	set_title();
	channel_menu.sel = 0;
	channel_menu.top = 0;
	ui_push(&channel_menu.screen);
}

static void create_done(const char *text)
{
	struct mesh_channel c = {.key_type = MESH_KEY_NUMBER};

	if (!text[0]) {
		return;
	}
	snprintf(c.name, sizeof(c.name), "%s", text);
	/* A number other than 1, which is the public default key. */
	c.number = 2 + k_cycle_get_32() % 254;
	mesh_set_channel(slot, &c);
	open_channel();
}

static void list_action(struct ui_menu *m, int idx)
{
	struct mesh_channel c;

	ARG_UNUSED(m);
	slot = idx + 1;
	if (mesh_get_channel(slot, &c)) {
		open_channel();
	} else {
		compose_open_edit("Channel name", "", MESH_CHANNEL_NAME, create_done);
	}
}

static const char *list_value(struct ui_menu *m, int idx, char *buf, size_t len)
{
	struct mesh_channel c;

	ARG_UNUSED(m);
	if (!mesh_get_channel(idx + 1, &c)) {
		return tr("new");
	}
	snprintf(buf, len, "#%s", c.name);
	return buf;
}

static const struct ui_menu_item list_items[] = {
	{"Channel 1", NULL, list_action},
	{"Channel 2", NULL, list_action},
	{"Channel 3", NULL, list_action},
};

BUILD_ASSERT(ARRAY_SIZE(list_items) == MESH_CHANNELS - 1);

struct ui_menu channels_menu = UI_MENU("Channels", list_items, list_value);
