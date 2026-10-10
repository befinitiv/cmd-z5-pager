/*
 * Host side of the emulator front panel: SDL2 window, rendering, keyboard and
 * mouse input, audio output.
 *
 * Built in the native simulator runner context with the host C library, so it
 * must not include Zephyr headers. Events are passed to the Zephyr side
 * through the callback given to fp_bottom_init(); the SDL event loop itself is
 * run by Zephyr's native_sim SDL support (boards/native/common/sdl).
 */

/* For localtime_r() and tm_gmtoff. */
#define _GNU_SOURCE

#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "frontpanel_bottom.h"
#include "gfx/font.h"
#include "font_small.h"
#include "font_text.h"

#define AUDIO_RATE 16000

struct button_def {
	SDL_Rect r;
	const char *label;
	const char *sub;
};

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *lcd_tex;
static SDL_AudioDeviceID audio_dev;
static fp_event_cb_t event_cb;
static int scale;

static struct button_def buttons[FP_BTN_COUNT];
static bool pressed[FP_BTN_COUNT];
static int mouse_button = -1;
static bool lid_closed;
static bool usb_plugged;
static int wheel_pos;
static SDL_Rect bezel_rect, lcd_rect, wheel_rect, lid_rect, ind_rect, usb_rect;

/* Indicator rows (LED, VIB, SPK, EAR, USB) in ind_rect. */
#define IND_LINE (9 * 2 + 6)
#define USB_ROW  4
static int win_w, win_h;
static volatile bool screenshot_requested;
static int screenshot_count;
static char window_shot_path[256];
static volatile bool window_shot_requested;

typedef struct {
	uint8_t r, g, b;
} rgb_t;

static const rgb_t col_bg = {0x24, 0x26, 0x29};
static const rgb_t col_bezel = {0x0e, 0x0f, 0x10};
static const rgb_t col_key = {0x4a, 0x4d, 0x52};
static const rgb_t col_key_pressed = {0x7c, 0x80, 0x88};
static const rgb_t col_label = {0xd8, 0xd8, 0xd8};
static const rgb_t col_dim = {0x80, 0x84, 0x88};
static const rgb_t col_green = {0x7c, 0xff, 0x5a};
/* Unlit STN panel, fully lit by the green LEDs, and the darkest pixel colour. */
static const rgb_t col_lcd_unlit = {0xa9, 0xb0, 0x9c};
static const rgb_t col_lcd_lit = {0x9d, 0xe8, 0x6e};
static const rgb_t col_lcd_ink = {0x16, 0x1e, 0x14};

static rgb_t mix(rgb_t a, rgb_t b, float t)
{
	if (t < 0) {
		t = 0;
	} else if (t > 1) {
		t = 1;
	}
	return (rgb_t){(uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t),
		       (uint8_t)(a.b + (b.b - a.b) * t)};
}

static void set_color(rgb_t c)
{
	SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, 0xff);
}

static void fill(const SDL_Rect *r, rgb_t c)
{
	set_color(c);
	SDL_RenderFillRect(renderer, r);
}

static void outline(const SDL_Rect *r, rgb_t c)
{
	set_color(c);
	SDL_RenderDrawRect(renderer, r);
}

static int text_width(const struct gfx_font *f, const char *s, int px)
{
	int w = 0;

	for (; *s; s++) {
		w += f->widths[gfx_font_glyph(f, (uint8_t)*s)];
	}
	return w * px;
}

/* Draw ASCII text with a gfx font, each font pixel px x px screen pixels. */
static void draw_text(const struct gfx_font *f, int x, int y, const char *s, int px, rgb_t c)
{
	set_color(c);
	for (; *s; s++) {
		uint16_t idx = gfx_font_glyph(f, (uint8_t)*s);
		int w = f->widths[idx];
		int stride = (w + 7) / 8;
		const uint8_t *bits = &f->bitmap[f->offsets[idx]];

		for (int row = 0; row < f->height; row++) {
			for (int col = 0; col < w; col++) {
				if (bits[row * stride + col / 8] & (0x80 >> (col % 8))) {
					SDL_Rect r = {x + col * px, y + row * px, px, px};

					SDL_RenderFillRect(renderer, &r);
				}
			}
		}
		x += w * px;
	}
}

static void draw_text_centered(const struct gfx_font *f, const SDL_Rect *r, int y, const char *s,
			       int px, rgb_t c)
{
	draw_text(f, r->x + (r->w - text_width(f, s, px)) / 2, y, s, px, c);
}

static void layout(void)
{
	static const char *const labels[FP_BTN_COUNT][2] = {
		[FP_BTN_1] = {"1", ".,?!"},   [FP_BTN_2] = {"2", "abc"},
		[FP_BTN_3] = {"3", "def"},    [FP_BTN_4] = {"4", "ghi"},
		[FP_BTN_5] = {"5", "jkl"},    [FP_BTN_6] = {"6", "mno"},
		[FP_BTN_7] = {"7", "pqrs"},   [FP_BTN_8] = {"8", "tuv"},
		[FP_BTN_9] = {"9", "wxyz"},   [FP_BTN_0] = {"0", "space"},
		[FP_BTN_STAR] = {"*", "Aa"},  [FP_BTN_POUND] = {"#", "mode"},
		[FP_BTN_CALL] = {"CALL", ""}, [FP_BTN_MEMO] = {"Memo", ""},
		[FP_BTN_CLEAR] = {"C", ""},   [FP_BTN_WHEEL_PUSH] = {"push", ""},
		[FP_BTN_WHEEL_UP] = {"back", ""}, [FP_BTN_WHEEL_DOWN] = {"bend dn", ""},
		[FP_BTN_POWER] = {"PWR", ""},
	};
	/* Keypad rows, top to bottom. */
	static const int grid[5][3] = {
		{FP_BTN_CALL, FP_BTN_MEMO, FP_BTN_CLEAR},
		{FP_BTN_1, FP_BTN_2, FP_BTN_3},
		{FP_BTN_4, FP_BTN_5, FP_BTN_6},
		{FP_BTN_7, FP_BTN_8, FP_BTN_9},
		{FP_BTN_STAR, FP_BTN_0, FP_BTN_POUND},
	};
	const int margin = 16, gap = 10, kh = 8 * scale;

	for (int i = 0; i < FP_BTN_COUNT; i++) {
		buttons[i].label = labels[i][0];
		buttons[i].sub = labels[i][1];
	}

	bezel_rect = (SDL_Rect){margin, margin, FP_LCD_W * scale + 24, FP_LCD_H * scale + 24};
	lcd_rect = (SDL_Rect){margin + 12, margin + 12, FP_LCD_W * scale, FP_LCD_H * scale};

	int pad_y = bezel_rect.y + bezel_rect.h + 16;
	int kw = (bezel_rect.w - 2 * gap) / 3;

	for (int row = 0; row < 5; row++) {
		for (int col = 0; col < 3; col++) {
			buttons[grid[row][col]].r = (SDL_Rect){margin + col * (kw + gap),
							       pad_y + row * (kh + gap), kw, kh};
		}
	}

	/* Right column: wheel controls, indicators, lid and power. */
	int x1 = bezel_rect.x + bezel_rect.w + 16;
	int cw = 24 * scale;
	int y = margin;

	buttons[FP_BTN_WHEEL_UP].r = (SDL_Rect){x1, y, cw, kh};
	y += kh + gap;
	wheel_rect = (SDL_Rect){x1, y, cw, 3 * kh};
	y += 3 * kh + gap;
	buttons[FP_BTN_WHEEL_PUSH].r = (SDL_Rect){x1, y, cw, kh};
	y += kh + gap;
	buttons[FP_BTN_WHEEL_DOWN].r = (SDL_Rect){x1, y, cw, kh};
	y += kh + gap;
	ind_rect = (SDL_Rect){x1, y, cw, pad_y + 3 * (kh + gap) - y - gap};
	usb_rect = (SDL_Rect){x1, y + USB_ROW * IND_LINE, cw, IND_LINE};
	lid_rect = (SDL_Rect){x1, pad_y + 3 * (kh + gap), cw, kh};
	buttons[FP_BTN_POWER].r = (SDL_Rect){x1, pad_y + 4 * (kh + gap), cw, kh};

	win_w = x1 + cw + margin;
	win_h = pad_y + 5 * kh + 4 * gap + margin;
}

static void emit(uint8_t type, uint8_t button, int32_t value)
{
	struct fp_event ev = {.type = type, .button = button, .value = value};

	if (event_cb) {
		event_cb(&ev);
	}
}

static void press(int b, bool down)
{
	if (b < 0 || b >= FP_BTN_COUNT || pressed[b] == down) {
		return;
	}
	pressed[b] = down;
	emit(FP_EV_BUTTON, b, down);
}

static void rotate(int detents)
{
	wheel_pos += detents;
	emit(FP_EV_WHEEL, 0, detents);
}

static void toggle_lid(void)
{
	lid_closed = !lid_closed;
	emit(FP_EV_LID, 0, lid_closed);
}

static void toggle_usb(void)
{
	usb_plugged = !usb_plugged;
	emit(FP_EV_USB, 0, usb_plugged);
}

void fp_bottom_inject(const struct fp_event *ev)
{
	switch (ev->type) {
	case FP_EV_BUTTON:
		press(ev->button, ev->value);
		break;
	case FP_EV_WHEEL:
		rotate(ev->value);
		break;
	case FP_EV_LID:
		if (lid_closed != (ev->value != 0)) {
			toggle_lid();
		}
		break;
	case FP_EV_USB:
		if (usb_plugged != (ev->value != 0)) {
			toggle_usb();
		}
		break;
	default:
		break;
	}
}

void fp_bottom_request_shot(const char *path)
{
	snprintf(window_shot_path, sizeof(window_shot_path), "%s", path);
	window_shot_requested = true;
}

bool fp_bottom_shot_pending(void)
{
	return window_shot_requested;
}

long fp_bottom_read_file(const char *path, char *buf, long size)
{
	FILE *f = fopen(path, "r");
	long n;

	if (!f) {
		return -1;
	}
	n = (long)fread(buf, 1, size - 1, f);
	buf[n] = '\0';
	fclose(f);
	return n;
}

static bool in_rect(const SDL_Rect *r, int x, int y)
{
	SDL_Point p = {x, y};

	return SDL_PointInRect(&p, r);
}

static int key_to_button(SDL_Keycode k)
{
	switch (k) {
	case SDLK_1: case SDLK_KP_1: return FP_BTN_1;
	case SDLK_2: case SDLK_KP_2: return FP_BTN_2;
	case SDLK_3: case SDLK_KP_3: return FP_BTN_3;
	case SDLK_4: case SDLK_KP_4: return FP_BTN_4;
	case SDLK_5: case SDLK_KP_5: return FP_BTN_5;
	case SDLK_6: case SDLK_KP_6: return FP_BTN_6;
	case SDLK_7: case SDLK_KP_7: return FP_BTN_7;
	case SDLK_8: case SDLK_KP_8: return FP_BTN_8;
	case SDLK_9: case SDLK_KP_9: return FP_BTN_9;
	case SDLK_0: case SDLK_KP_0: return FP_BTN_0;
	case SDLK_ASTERISK: case SDLK_KP_MULTIPLY: case SDLK_PLUS: case SDLK_KP_PLUS:
		return FP_BTN_STAR;
	case SDLK_HASH: case SDLK_KP_DIVIDE: case SDLK_BACKSLASH: case SDLK_KP_MINUS:
		return FP_BTN_POUND;
	case SDLK_RETURN: case SDLK_KP_ENTER: return FP_BTN_CALL;
	case SDLK_BACKSPACE: case SDLK_DELETE: return FP_BTN_CLEAR;
	case SDLK_m: return FP_BTN_MEMO;
	case SDLK_SPACE: return FP_BTN_WHEEL_PUSH;
	case SDLK_PAGEUP: case SDLK_LEFT: return FP_BTN_WHEEL_UP;
	case SDLK_PAGEDOWN: case SDLK_RIGHT: return FP_BTN_WHEEL_DOWN;
	case SDLK_ESCAPE: return FP_BTN_POWER;
	default: return -1;
	}
}

static int event_watch(void *userdata, SDL_Event *e)
{
	(void)userdata;

	switch (e->type) {
	case SDL_KEYDOWN:
	case SDL_KEYUP: {
		bool down = e->type == SDL_KEYDOWN;
		SDL_Keycode k = e->key.keysym.sym;

		if (down && (k == SDLK_UP || k == SDLK_DOWN)) {
			rotate(k == SDLK_UP ? 1 : -1);
		} else if (e->key.repeat) {
			break;
		} else if (down && k == SDLK_l) {
			toggle_lid();
		} else if (down && k == SDLK_u) {
			toggle_usb();
		} else if (down && k == SDLK_F12) {
			screenshot_requested = true;
		} else {
			press(key_to_button(k), down);
		}
		break;
	}
	case SDL_MOUSEWHEEL: {
		int y = e->wheel.y;

		if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
			y = -y;
		}
		if (y) {
			rotate(y);
		}
		break;
	}
	case SDL_MOUSEBUTTONDOWN: {
		int x = e->button.x, y = e->button.y;

		if (e->button.button == SDL_BUTTON_MIDDLE) {
			press(FP_BTN_WHEEL_PUSH, true);
			break;
		}
		if (e->button.button != SDL_BUTTON_LEFT) {
			break;
		}
		if (in_rect(&usb_rect, x, y)) {
			toggle_usb();
			break;
		}
		if (in_rect(&lid_rect, x, y)) {
			toggle_lid();
			break;
		}
		if (in_rect(&wheel_rect, x, y)) {
			rotate(y < wheel_rect.y + wheel_rect.h / 2 ? 1 : -1);
			break;
		}
		for (int b = 0; b < FP_BTN_COUNT; b++) {
			if (in_rect(&buttons[b].r, x, y)) {
				mouse_button = b;
				press(b, true);
				break;
			}
		}
		break;
	}
	case SDL_MOUSEBUTTONUP:
		if (e->button.button == SDL_BUTTON_MIDDLE) {
			press(FP_BTN_WHEEL_PUSH, false);
		} else if (e->button.button == SDL_BUTTON_LEFT && mouse_button >= 0) {
			press(mouse_button, false);
			mouse_button = -1;
		}
		break;
	default:
		break;
	}
	return 1;
}

int fp_bottom_init(int s, fp_event_cb_t cb)
{
	scale = s;
	event_cb = cb;
	layout();

	window = SDL_CreateWindow("CMD-Z5 emulator", SDL_WINDOWPOS_UNDEFINED,
				  SDL_WINDOWPOS_UNDEFINED, win_w, win_h, 0);
	if (!window) {
		fprintf(stderr, "frontpanel: SDL_CreateWindow failed: %s\n", SDL_GetError());
		return -1;
	}
	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
	if (!renderer) {
		renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
	}
	if (!renderer) {
		fprintf(stderr, "frontpanel: SDL_CreateRenderer failed: %s\n", SDL_GetError());
		return -1;
	}
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
	lcd_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
				    SDL_TEXTUREACCESS_STREAMING, FP_LCD_W, FP_LCD_H);
	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

	SDL_AddEventWatch(event_watch, NULL);

	if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
		SDL_AudioSpec want = {.freq = AUDIO_RATE, .format = AUDIO_S16SYS, .channels = 1,
				      .samples = 512};
		SDL_AudioSpec have;

		audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
		if (audio_dev) {
			SDL_PauseAudioDevice(audio_dev, 0);
		}
	}
	if (!audio_dev) {
		fprintf(stderr, "frontpanel: no audio output (%s)\n", SDL_GetError());
	}

	printf("CMD-Z5 emulator keys: 0-9 * (+) # (\\), Enter = CALL, Backspace = C, M = Memo,\n"
	       "  Up/Down or mouse wheel = rotate, Space or middle click = wheel push,\n"
	       "  Left or PgUp = wheel bend up (back), Right or PgDn = wheel bend down,\n"
	       "  L = lid, Esc = power, U = USB power,\n"
	       "  F12 = screenshot of the display\n");
	return 0;
}

static void render_lcd(const struct fp_state *st, int jx, int jy)
{
	rgb_t base = mix(col_lcd_unlit, col_lcd_lit, st->backlight / 100.0f);
	/* 0x85 is the phone's default contrast; scale the darkness around it. */
	float k = st->contrast / (float)0x85;
	uint32_t palette[4];
	void *pixels;
	int pitch;

	for (int l = 0; l < 4; l++) {
		rgb_t c = st->lcd_on ? mix(base, col_lcd_ink, l / 3.0f * k) : base;

		palette[l] = 0xff000000u | (c.r << 16) | (c.g << 8) | c.b;
	}

	if (SDL_LockTexture(lcd_tex, NULL, &pixels, &pitch) == 0) {
		for (int y = 0; y < FP_LCD_H; y++) {
			uint32_t *line = (uint32_t *)((uint8_t *)pixels + y * pitch);

			for (int x = 0; x < FP_LCD_W; x++) {
				uint8_t b = st->fb[y * FP_LCD_STRIDE + x / 4];

				line[x] = palette[(b >> (6 - 2 * (x % 4))) & 3];
			}
		}
		SDL_UnlockTexture(lcd_tex);
	}

	SDL_Rect r = lcd_rect;

	r.x += jx;
	r.y += jy;
	SDL_RenderCopy(renderer, lcd_tex, NULL, &r);

	/* Faint pixel grid, like the gaps between the LCD's pixels. */
	if (scale >= 4) {
		SDL_SetRenderDrawColor(renderer, base.r, base.g, base.b, 0x50);
		for (int x = 1; x < FP_LCD_W; x++) {
			SDL_RenderDrawLine(renderer, r.x + x * scale, r.y, r.x + x * scale,
					   r.y + r.h - 1);
		}
		for (int y = 1; y < FP_LCD_H; y++) {
			SDL_RenderDrawLine(renderer, r.x, r.y + y * scale, r.x + r.w - 1,
					   r.y + y * scale);
		}
	}
}

static bool under_lid(int b)
{
	return b != FP_BTN_WHEEL_PUSH && b != FP_BTN_WHEEL_UP && b != FP_BTN_WHEEL_DOWN &&
	       b != FP_BTN_POWER;
}

static void render_button(int b, rgb_t label_col)
{
	const struct button_def *d = &buttons[b];
	bool big = d->sub[0] != '\0';
	int px = big ? 3 : 2;
	const struct gfx_font *f = &font_text;
	rgb_t key_col = pressed[b] ? col_key_pressed : col_key;

	/* The closed lid covers the keypad; drawn dimmed (clicks still go through). */
	if (lid_closed && under_lid(b)) {
		key_col = mix(key_col, col_bg, 0.6f);
		label_col = mix(col_dim, col_bg, 0.5f);
	}
	fill(&d->r, key_col);
	outline(&d->r, col_bezel);
	if (big) {
		int y = d->r.y + (d->r.h - f->height * px - font_small.height * 2 - 2) / 2;

		draw_text_centered(f, &d->r, y, d->label, px, label_col);
		draw_text_centered(&font_small, &d->r, y + f->height * px + 2, d->sub, 2,
				   mix(label_col, col_key, 0.35f));
	} else {
		draw_text_centered(f, &d->r, d->r.y + (d->r.h - f->height * px) / 2, d->label, px,
				   label_col);
	}
}

static void render_indicator(int row, const char *name, rgb_t on_col, float level)
{
	SDL_Rect dot = {ind_rect.x + 4, ind_rect.y + 4 + row * IND_LINE, 14, 14};

	fill(&dot, mix((rgb_t){0x30, 0x32, 0x35}, on_col, level));
	outline(&dot, col_bezel);
	draw_text(&font_text, dot.x + 24, dot.y, name, 2, level > 0 ? col_label : col_dim);
}

/* Save a part of the rendered frame (NULL: whole window) as BMP. */
static void save_bmp(const SDL_Rect *area, const char *name)
{
	int w = area ? area->w : win_w;
	int h = area ? area->h : win_h;
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);

	if (!s) {
		return;
	}
	if (SDL_RenderReadPixels(renderer, area, SDL_PIXELFORMAT_ARGB8888, s->pixels,
				 s->pitch) == 0 &&
	    SDL_SaveBMP(s, name) == 0) {
		printf("frontpanel: saved %s\n", name);
	} else {
		fprintf(stderr, "frontpanel: saving %s failed: %s\n", name, SDL_GetError());
	}
	SDL_FreeSurface(s);
}

static void save_screenshot(void)
{
	char name[64];

	snprintf(name, sizeof(name), "cmdz5-screen-%03d.bmp", screenshot_count++);
	save_bmp(&lcd_rect, name);
}

void fp_bottom_render(const struct fp_state *st)
{
	int jx = 0, jy = 0;

	if (!renderer) {
		return;
	}
	if (st->vibration > 0) {
		jx = (st->frame & 1) ? 2 : -2;
		jy = (st->frame & 2) ? 1 : -1;
	}

	fill(&(SDL_Rect){0, 0, win_w, win_h}, col_bg);

	SDL_Rect bezel = bezel_rect;

	bezel.x += jx;
	bezel.y += jy;
	fill(&bezel, col_bezel);
	render_lcd(st, jx, jy);
	if (screenshot_requested && jx == 0) {
		save_screenshot();
		screenshot_requested = false;
	}

	/* Keypad labels glow green with the keypad backlight (same LED switch). */
	rgb_t label_col = mix(col_label, col_green, st->backlight / 100.0f);

	for (int b = 0; b < FP_BTN_COUNT; b++) {
		render_button(b, b == FP_BTN_POWER ? col_label : label_col);
	}

	/* Wheel with moving ridges. */
	fill(&wheel_rect, (rgb_t){0x3a, 0x3c, 0x40});
	set_color((rgb_t){0x22, 0x23, 0x25});
	int step = 3 * scale;
	int off = ((wheel_pos * scale) % step + step) % step;

	for (int y = wheel_rect.y + off; y < wheel_rect.y + wheel_rect.h; y += step) {
		SDL_RenderDrawLine(renderer, wheel_rect.x + 6, y, wheel_rect.x + wheel_rect.w - 7, y);
	}
	outline(&wheel_rect, col_bezel);

	render_indicator(0, "LED", col_green, st->backlight / 100.0f);
	render_indicator(1, "VIB", (rgb_t){0xff, 0xa0, 0x30}, st->vibration / 100.0f);
	render_indicator(2, "SPK", (rgb_t){0x60, 0xb0, 0xff}, st->speaker_active);
	render_indicator(3, "EAR", (rgb_t){0x60, 0xb0, 0xff}, st->earpiece_active);
	/* USB power: click (or U) plugs or unplugs it for the simulated battery. */
	render_indicator(USB_ROW, "USB", (rgb_t){0xff, 0xe0, 0x40}, st->usb);

	fill(&lid_rect, lid_closed ? (rgb_t){0x90, 0x50, 0x30} : col_key);
	outline(&lid_rect, col_bezel);
	draw_text_centered(&font_text, &lid_rect, lid_rect.y + (lid_rect.h - 14) / 2,
			   lid_closed ? "lid closed" : "lid open", 2, col_label);

	if (window_shot_requested) {
		save_bmp(NULL, window_shot_path);
		window_shot_requested = false;
	}
	SDL_RenderPresent(renderer);
}

int fp_bottom_audio_rate(void)
{
	return AUDIO_RATE;
}

void fp_bottom_audio_queue(const int16_t *samples, int count)
{
	if (audio_dev) {
		SDL_QueueAudio(audio_dev, samples, count * sizeof(int16_t));
	}
}

void fp_bottom_audio_clear(void)
{
	if (audio_dev) {
		SDL_ClearQueuedAudio(audio_dev);
	}
}

int64_t fp_bottom_unix_time(void)
{
	return (int64_t)time(NULL);
}

int32_t fp_bottom_utc_offset(void)
{
	time_t now = time(NULL);
	struct tm tm;

	localtime_r(&now, &tm);
	return (int32_t)tm.tm_gmtoff;
}
