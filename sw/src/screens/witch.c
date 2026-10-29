/*
 * "Hexenflug": a witch on her broom flies through the night sky. The wheel
 * sets her height; bats, crows, church towers and storm clouds scroll in
 * from the right and must be avoided, stars give bonus points. Three hits
 * end the flight. The pace rises slowly; the best score is stored.
 *
 * Keys: wheel (or 2/8) up and down, CALL pause, # music on/off,
 * C or wheel bent up leaves.
 *
 * Positions are in 1/16 px; the game runs at 30 steps per second. An own
 * tune loops in the background (hw_melody), effects are single tones
 * (hw_tone) that sound alongside it.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include "../app.h"
#include "../config.h"
#include "../hw.h"
#include "../power.h"
#include "screens.h"

#define FRAME_MS 33
#define SUB      16 /* subpixels per pixel */
#define TOP_Y    8  /* playfield below the score line */
#define BOTTOM_Y GFX_HEIGHT

#define WITCH_X     10
#define WITCH_W     12
#define WITCH_H     10
#define WHEEL_STEP  (4 * SUB) /* height change per wheel detent */
#define KEY_STEP    (2 * SUB) /* per frame with 2/8 held */
#define CLIMB_V     (3 * SUB / 2) /* the witch follows the wheel this fast */

#define START_SPEED  SUB       /* scrolling, per frame */
#define MAX_SPEED    (3 * SUB)
#define SPEEDUP_FRAMES 150     /* a little faster every 5 s */
#define MIN_GAP      (30 * SUB) /* scrolled distance between two spawns */

#define START_LIVES  3
#define HURT_FRAMES  45
#define STAR_POINTS  50
#define HI_SCORE_KEY "game/witch_hi"

#define MAX_THINGS 8

/* Sprites ------------------------------------------------------------------- */

/* '.' transparent, 'w' white, 'l' light, 'd' dark, 'b' black; any width. */
typedef const char *const art_t[];

static const uint8_t art_colors[128] = {
	['w'] = GFX_WHITE, ['l'] = GFX_LIGHT, ['d'] = GFX_DARK, ['b'] = GFX_BLACK,
};

/* Facing right: pointed hat, hair and cape behind, the broom under her. */
static art_t witch_a = {
	"....b.......", "....bb......", "...bbbb.....", ".bbbbbbbb...",
	"...dlbl.....", "...dllll....", "..ddbbbl....", ".dd.bbbb....",
	"dddbbbbbbbbb", "....b.b.....",
};
static art_t witch_b = {
	"....b.......", "....bb......", "...bbbb.....", ".bbbbbbbb...",
	"...dlbl.....", "..ddllll....", ".dddbbbl....", "..d.bbbb....",
	"ldddbbbbbbbb", ".....b.b....",
};
static art_t bat_a = {
	"b......b", "bb.bb.bb", "bbbbbbbb", ".bwbbwb.", "..bbbb..", "........",
};
static art_t bat_b = {
	"........", "...bb...", ".bbbbbb.", "bbwbbwbb", "b.bbbb.b", "b......b",
};
/* Flying left, beak first. */
static art_t crow_a = {
	"..bb....", ".bwbb...", "ddbbbbbb", "...bbbbd", "....bb..", "........",
};
static art_t crow_b = {
	".....bb.", "..bbbb..", ".bwbbbb.", "ddbbbbbb", "....bb..", "........",
};
static art_t star_a = {
	"..d..", ".ddd.", "ddddd", ".ddd.", ".d.d.",
};
static art_t star_b = {
	"..b..", ".bdb.", "bdddb", ".bdb.", ".b.b.",
};
static art_t heart = {
	".b.b.", "bbbbb", "bbbbb", ".bbb.", "..b..",
};

#define ART_H(a) ((int)ARRAY_SIZE(a))
#define ART_W(a) ((int)strlen((a)[0]))

static void blit(struct gfx *g, int x, int y, const char *const *art, int h, bool blink)
{
	if (blink) {
		return;
	}
	for (int r = 0; r < h; r++) {
		int sy = y + r;

		if (sy < TOP_Y || sy >= BOTTOM_Y) {
			continue;
		}
		for (int c = 0; art[r][c]; c++) {
			char p = art[r][c];
			int sx = x + c;

			if (p != '.' && sx >= 0 && sx < GFX_WIDTH) {
				gfx_pixel(g, sx, sy, art_colors[(uint8_t)p]);
			}
		}
	}
}

/* Music and sounds ---------------------------------------------------------- */

/* An own tune, D minor, 140 bpm: a sly little witch's dance in two phrases. */
#define E8 200
#define Q4 400
static const struct hw_note tune[] = {
	/* A */
	{440, E8}, {587, E8}, {698, E8}, {659, E8}, {587, Q4}, {440, Q4},
	{466, E8}, {587, E8}, {784, E8}, {698, E8}, {659, Q4}, {0, Q4},
	{440, E8}, {554, E8}, {659, E8}, {784, E8}, {698, E8}, {659, E8}, {587, E8}, {554, E8},
	{587, Q4}, {440, Q4}, {294, Q4}, {0, Q4},
	/* B */
	{698, E8}, {698, E8}, {659, E8}, {587, E8}, {523, Q4}, {440, Q4},
	{466, E8}, {523, E8}, {587, E8}, {659, E8}, {698, Q4}, {587, Q4},
	{784, E8}, {698, E8}, {659, E8}, {587, E8}, {554, E8}, {659, E8}, {880, Q4},
	{587, Q4}, {0, E8}, {294, E8}, {587, Q4}, {0, Q4},
};

static uint32_t tune_ms(void)
{
	uint32_t ms = 0;

	for (size_t i = 0; i < ARRAY_SIZE(tune); i++) {
		ms += tune[i].ms;
	}
	return ms;
}

static const struct hw_note snd_over[] = {{392, 200}, {330, 200}, {262, 200}, {196, 500}};

/* State --------------------------------------------------------------------- */

enum thing_type { T_BAT, T_CROW, T_STAR, T_TOWER, T_CLOUD };

struct thing {
	bool on;
	uint8_t type;
	int32_t x;  /* subpixels */
	int16_t y;  /* px (towers: top of the roof; clouds: bottom edge) */
	int16_t w, h;
	int16_t phase;
};

enum state { ST_TITLE, ST_PLAY, ST_OVER };

static struct {
	enum state state;
	bool paused;
	bool music;
	uint32_t frame;
	int32_t y, target; /* witch top, subpixels */
	int32_t speed;
	int32_t scrolled;  /* total, subpixels */
	int32_t next_spawn;
	int lives;
	int hurt;          /* frames of blinking after a hit */
	uint32_t score;
	uint32_t hi;
	uint32_t over_frame;
	struct thing things[MAX_THINGS];
	uint32_t rnd;
} wg = {.music = true};

static uint32_t last_ms, music_at;
static bool key_up, key_down;

static uint32_t rnd(uint32_t n)
{
	/* xorshift32 */
	wg.rnd ^= wg.rnd << 13;
	wg.rnd ^= wg.rnd >> 17;
	wg.rnd ^= wg.rnd << 5;
	return wg.rnd % n;
}

static void music_start(void)
{
	if (wg.music) {
		hw_melody(HW_SPEAKER, tune, ARRAY_SIZE(tune), app_config.volume / 2);
		music_at = k_uptime_get_32();
	}
}

static void music_poll(void)
{
	if (wg.music && wg.state == ST_PLAY && !wg.paused &&
	    k_uptime_get_32() - music_at >= tune_ms()) {
		music_start();
	}
}

static void music_stop(void)
{
	hw_audio_stop();
}

static void save_hi(void)
{
	if (wg.score > wg.hi) {
		wg.hi = wg.score;
		settings_save_one(HI_SCORE_KEY, &wg.hi, sizeof(wg.hi));
	}
}

static void new_game(void)
{
	memset(wg.things, 0, sizeof(wg.things));
	wg.state = ST_PLAY;
	wg.paused = false;
	wg.frame = 0;
	wg.y = wg.target = ((TOP_Y + BOTTOM_Y - WITCH_H) / 2) * SUB;
	wg.speed = START_SPEED;
	wg.scrolled = 0;
	wg.next_spawn = 40 * SUB;
	wg.lives = START_LIVES;
	wg.hurt = 0;
	wg.score = 0;
	wg.rnd ^= k_cycle_get_32() | 1;
	music_start();
}

/* Spawning ------------------------------------------------------------------ */

static void spawn(void)
{
	struct thing *t = NULL;
	uint32_t r = rnd(100);
	/* Towers and clouds become more frequent with the pace. */
	int big = 20 + (wg.speed - START_SPEED) * 30 / (MAX_SPEED - START_SPEED);

	for (int i = 0; i < MAX_THINGS; i++) {
		if (!wg.things[i].on) {
			t = &wg.things[i];
			break;
		}
	}
	if (!t) {
		return;
	}
	*t = (struct thing){.on = true, .x = GFX_WIDTH * SUB, .phase = rnd(64)};
	if (r < (uint32_t)big / 2) {
		t->type = T_TOWER;
		t->w = 9;
		t->h = 14 + rnd(18);
		t->y = BOTTOM_Y - t->h;
	} else if (r < (uint32_t)big) {
		t->type = T_CLOUD;
		t->w = 18 + rnd(8);
		t->h = 8 + rnd(12);
		t->y = TOP_Y + t->h;
	} else if (r < 70) {
		t->type = T_BAT;
		t->w = 8;
		t->h = 6;
		t->y = TOP_Y + 6 + rnd(BOTTOM_Y - TOP_Y - 18);
	} else if (r < 88) {
		t->type = T_CROW;
		t->w = 8;
		t->h = 6;
		t->y = TOP_Y + 2 + rnd(BOTTOM_Y - TOP_Y - 10);
	} else {
		t->type = T_STAR;
		t->w = 5;
		t->h = 5;
		t->y = TOP_Y + 4 + rnd(BOTTOM_Y - TOP_Y - 10);
	}
}

/* Where a thing is on the screen this frame (bats bob, crows are faster). */
static void thing_box(const struct thing *t, int *x, int *y, int *w, int *h)
{
	static const int8_t bob[] = {0, 1, 2, 3, 3, 2, 1, 0, -1, -2, -3, -3, -2, -1};

	*x = t->x / SUB;
	*w = t->w;
	*h = t->h;
	switch (t->type) {
	case T_BAT:
		*y = t->y + bob[((wg.frame + t->phase) / 3) % ARRAY_SIZE(bob)];
		break;
	case T_CLOUD:
		*y = TOP_Y;
		break;
	default:
		*y = t->y;
		break;
	}
}

static bool hits(const struct thing *t)
{
	int x, y, w, h;
	/* The witch, a bit smaller than her picture (hat tip and broom end). */
	int wx = WITCH_X + 2, wy = wg.y / SUB + 1, ww = WITCH_W - 4, wh = WITCH_H - 2;

	thing_box(t, &x, &y, &w, &h);
	if (t->type == T_TOWER) {
		/* The roof is a narrow spire: only its lower half counts fully. */
		y += 4;
		h -= 4;
	}
	return wx < x + w - 1 && x + 1 < wx + ww && wy < y + h - 1 && y + 1 < wy + wh;
}

/* Game step ----------------------------------------------------------------- */

static void step(void)
{
	wg.frame++;
	if (wg.state != ST_PLAY) {
		return;
	}

	/* Witch: follows the wheel target, 2/8 move it continuously. */
	if (key_up) {
		wg.target -= KEY_STEP;
	}
	if (key_down) {
		wg.target += KEY_STEP;
	}
	wg.target = CLAMP(wg.target, TOP_Y * SUB, (BOTTOM_Y - WITCH_H) * SUB);
	if (wg.y < wg.target) {
		wg.y = MIN(wg.y + CLIMB_V, wg.target);
	} else if (wg.y > wg.target) {
		wg.y = MAX(wg.y - CLIMB_V, wg.target);
	}

	/* Pace and distance points. */
	if (wg.frame % SPEEDUP_FRAMES == 0 && wg.speed < MAX_SPEED) {
		wg.speed++;
	}
	wg.scrolled += wg.speed;
	if (wg.frame % 10 == 0) {
		wg.score++;
	}
	if (wg.hurt) {
		wg.hurt--;
	}

	/* Things move left, are collected or hit the witch. */
	for (int i = 0; i < MAX_THINGS; i++) {
		struct thing *t = &wg.things[i];

		if (!t->on) {
			continue;
		}
		t->x -= wg.speed + (t->type == T_CROW ? SUB / 2 : 0);
		if (t->x < -t->w * SUB) {
			t->on = false;
			continue;
		}
		if (!hits(t)) {
			continue;
		}
		if (t->type == T_STAR) {
			t->on = false;
			wg.score += STAR_POINTS;
			hw_tone(HW_SPEAKER, 1568, 60, app_config.volume);
			hw_tone(HW_SPEAKER, 2093, 120, app_config.volume / 2);
			continue;
		}
		if (wg.hurt) {
			continue;
		}
		wg.lives--;
		wg.hurt = HURT_FRAMES;
		if (app_config.vibrate) {
			hw_vibrate(150);
		}
		if (wg.lives <= 0) {
			wg.state = ST_OVER;
			wg.over_frame = wg.frame;
			music_stop();
			hw_melody(HW_SPEAKER, snd_over, ARRAY_SIZE(snd_over), app_config.volume);
			save_hi();
			return;
		}
		/* Over the music: a low, dissonant pair of tones. */
		hw_tone(HW_SPEAKER, 165, 200, app_config.volume);
		hw_tone(HW_SPEAKER, 175, 200, app_config.volume);
	}

	if (wg.scrolled >= wg.next_spawn) {
		spawn();
		/* Less room between things as the pace rises. */
		wg.next_spawn = wg.scrolled + MIN_GAP + rnd(40) * SUB -
				(wg.speed - START_SPEED) * 6;
	}
}

/* Drawing ------------------------------------------------------------------- */

/* Hills far behind: a smooth outline repeating every 128 px, scrolled slowly. */
static int hill_height(int x)
{
	static const uint8_t h[] = {
		6, 7, 8, 9, 10, 10, 11, 11, 10, 10, 9, 8, 7, 6, 6, 5,
		5, 5, 6, 7, 8, 10, 12, 13, 14, 14, 13, 12, 10, 8, 7, 6,
	};

	x &= 127;
	int i = x / 4, f = x % 4;
	int a = h[i], b = h[(i + 1) % ARRAY_SIZE(h)];

	return a + (b - a) * f / 4;
}

static void draw_background(struct gfx *g)
{
	int hills = wg.scrolled / SUB / 4;
	int sky = wg.scrolled / SUB / 8;

	gfx_fill_rect(g, 0, TOP_Y, GFX_WIDTH, BOTTOM_Y - TOP_Y, GFX_WHITE);

	/* Twinkling stars far away. */
	for (int i = 0; i < 12; i++) {
		int x = (i * 37 + 11 - sky) % GFX_WIDTH;
		int y = TOP_Y + 3 + (i * 23) % 34;

		if (x < 0) {
			x += GFX_WIDTH;
		}
		if ((wg.frame / 10 + i) % 5) {
			gfx_pixel(g, x, y, GFX_LIGHT);
		}
	}

	/* The moon: a light disc with a darker crater. */
	for (int dy = -5; dy <= 5; dy++) {
		for (int dx = -5; dx <= 5; dx++) {
			if (dx * dx + dy * dy <= 26) {
				gfx_pixel(g, 80 + dx, TOP_Y + 9 + dy, GFX_LIGHT);
			}
		}
	}
	gfx_pixel(g, 78, TOP_Y + 8, GFX_DARK);
	gfx_pixel(g, 82, TOP_Y + 11, GFX_DARK);

	for (int x = 0; x < GFX_WIDTH; x++) {
		int h = hill_height(x + hills);

		gfx_vline(g, x, BOTTOM_Y - h, h, GFX_LIGHT);
	}
}

static void draw_tower(struct gfx *g, int x, const struct thing *t)
{
	int roof = 8;
	int body_y = t->y + roof;

	/* Spire: a triangle from 1 px at the top to the full width. */
	for (int r = 0; r < roof; r++) {
		int half = (r * (t->w / 2)) / (roof - 1);

		gfx_hline(g, x + t->w / 2 - half, t->y + r, 2 * half + 1, GFX_BLACK);
	}
	gfx_fill_rect(g, x, body_y, t->w, BOTTOM_Y - body_y, GFX_DARK);
	gfx_vline(g, x, body_y, BOTTOM_Y - body_y, GFX_BLACK);
	gfx_vline(g, x + t->w - 1, body_y, BOTTOM_Y - body_y, GFX_BLACK);
	/* A lit window. */
	gfx_fill_rect(g, x + t->w / 2 - 1, body_y + 3, 3, 4, GFX_WHITE);
}

static void draw_cloud(struct gfx *g, int x, const struct thing *t)
{
	int bottom = t->y;

	gfx_fill_rect(g, x, TOP_Y, t->w, bottom - TOP_Y - 3, GFX_DARK);
	/* A bumpy lower edge. */
	for (int c = 0; c < t->w; c++) {
		int bump = (c + t->phase) % 6 < 3 ? 3 : 1;

		gfx_vline(g, x + c, bottom - 3, bump, GFX_DARK);
	}
	/* Now and then a lightning flash below it (looks only, it does not hit). */
	if ((wg.frame + t->phase) % 50 < 3) {
		int lx = x + t->w / 2;

		gfx_vline(g, lx, bottom, 3, GFX_BLACK);
		gfx_vline(g, lx + 1, bottom + 3, 3, GFX_BLACK);
	}
}

static void draw_things(struct gfx *g)
{
	for (int i = 0; i < MAX_THINGS; i++) {
		const struct thing *t = &wg.things[i];
		int x, y, w, h;
		bool flap = (wg.frame / 4 + t->phase) & 1;

		if (!t->on) {
			continue;
		}
		thing_box(t, &x, &y, &w, &h);
		switch (t->type) {
		case T_BAT:
			blit(g, x, y, flap ? bat_a : bat_b, ART_H(bat_a), false);
			break;
		case T_CROW:
			blit(g, x, y, flap ? crow_a : crow_b, ART_H(crow_a), false);
			break;
		case T_STAR:
			blit(g, x, y, (wg.frame / 6) & 1 ? star_a : star_b, ART_H(star_a), false);
			break;
		case T_TOWER:
			draw_tower(g, x, t);
			break;
		case T_CLOUD:
			draw_cloud(g, x, t);
			break;
		}
	}
}

static void draw_hud(struct gfx *g)
{
	char buf[16];

	gfx_fill_rect(g, 0, 0, GFX_WIDTH, TOP_Y, GFX_WHITE);
	snprintf(buf, sizeof(buf), "%05u", (unsigned int)wg.score);
	gfx_text(g, gfx_font_small, 1, 1, buf, GFX_BLACK);
	for (int i = 0; i < wg.lives; i++) {
		int x = GFX_WIDTH - 6 - i * 6;

		for (int r = 0; r < ART_H(heart); r++) {
			for (int c = 0; heart[r][c]; c++) {
				if (heart[r][c] != '.') {
					gfx_pixel(g, x + c, 1 + r, GFX_BLACK);
				}
			}
		}
	}
	gfx_hline(g, 0, TOP_Y - 1, GFX_WIDTH, GFX_LIGHT);
}

static void draw_box(struct gfx *g, int y, int h, const char *const *lines, int n)
{
	gfx_fill_rect(g, 8, y, GFX_WIDTH - 16, h, GFX_WHITE);
	gfx_rect(g, 8, y, GFX_WIDTH - 16, h, GFX_BLACK);
	for (int i = 0; i < n; i++) {
		gfx_text_aligned(g, i == 0 ? gfx_font_text : gfx_font_small, 8,
				 y + 3 + i * 8 + (i ? 1 : 0), GFX_WIDTH - 16, GFX_ALIGN_CENTER,
				 lines[i], GFX_BLACK);
	}
}

static void witch_draw(struct ui_screen *s, struct gfx *g)
{
	char line[20];
	bool blink = wg.hurt && (wg.hurt / 3) % 2;

	ARG_UNUSED(s);
	draw_background(g);
	draw_things(g);
	blit(g, WITCH_X, wg.y / SUB, (wg.frame / 5) & 1 ? witch_a : witch_b, WITCH_H, blink);
	draw_hud(g);

	switch (wg.state) {
	case ST_TITLE: {
		snprintf(line, sizeof(line), "HI %05u", (unsigned int)wg.hi);
		const char *const l[] = {"HEXENFLUG", "push: start", line, "wheel: up/down"};

		draw_box(g, 14, 40, l, ARRAY_SIZE(l));
		break;
	}
	case ST_OVER: {
		snprintf(line, sizeof(line), "HI %05u", (unsigned int)wg.hi);
		const char *const l[] = {"GAME OVER", line, "push: again"};

		draw_box(g, 20, 30, l, ARRAY_SIZE(l));
		break;
	}
	default:
		if (wg.paused) {
			const char *const l[] = {"PAUSE", "CALL: continue", wg.music ? "#: music off"
										   : "#: music on"};

			draw_box(g, 20, 30, l, ARRAY_SIZE(l));
		}
		break;
	}
}

/* Screen -------------------------------------------------------------------- */

static bool witch_event(struct ui_screen *s, const struct ui_event *ev)
{
	ARG_UNUSED(s);

	if (ev->type == UI_EV_WHEEL) {
		if (wg.state == ST_PLAY && !wg.paused) {
			wg.target -= ev->value * WHEEL_STEP; /* up = higher = smaller y */
		}
		return true;
	}
	if (ev->type != UI_EV_KEY_PRESS && ev->type != UI_EV_KEY_RELEASE &&
	    ev->type != UI_EV_KEY_LONG) {
		return false;
	}
	/* C and wheel bent up leave the game (default handling). */
	if (ev->key == UI_KEY_CLEAR || ev->key == UI_KEY_BACK) {
		return false;
	}
	if (ev->key == UI_KEY_2 || ev->key == UI_KEY_8) {
		bool down = ev->type != UI_EV_KEY_RELEASE;

		*(ev->key == UI_KEY_2 ? &key_up : &key_down) = down;
		return true;
	}
	if (ev->type != UI_EV_KEY_PRESS) {
		return true;
	}

	switch (wg.state) {
	case ST_TITLE:
		if (ev->key == UI_KEY_SELECT || ev->key == UI_KEY_5) {
			new_game();
		}
		break;
	case ST_OVER:
		/* A short wait, so a key pressed in the crash does not restart. */
		if ((ev->key == UI_KEY_SELECT || ev->key == UI_KEY_5) &&
		    wg.frame - wg.over_frame > 30) {
			new_game();
		}
		break;
	default:
		if (ev->key == UI_KEY_CALL) {
			wg.paused = !wg.paused;
			if (wg.paused) {
				music_stop();
			} else {
				music_start();
			}
		} else if (ev->key == UI_KEY_POUND) {
			wg.music = !wg.music;
			if (wg.music && !wg.paused) {
				music_start();
			} else {
				music_stop();
			}
		}
		break;
	}
	ui_invalidate();
	return true;
}

static void witch_poll(struct ui_screen *s)
{
	uint32_t now = k_uptime_get_32();
	int steps = 0;

	ARG_UNUSED(s);
	if (!power_display_on() || wg.paused) {
		last_ms = now;
		return;
	}
	while (now - last_ms >= FRAME_MS && steps < 4) {
		last_ms += FRAME_MS;
		step();
		steps++;
	}
	if (now - last_ms >= FRAME_MS) {
		/* Too far behind (e.g. a long redraw): skip, do not catch up. */
		last_ms = now;
	}
	if (steps) {
		ui_invalidate();
		/* Playing counts as activity: keep the backlight on. */
		if (wg.state == ST_PLAY && wg.frame % 30 == 0) {
			power_user_activity(false);
		}
	}
	music_poll();
}

static void witch_enter(struct ui_screen *s)
{
	ARG_UNUSED(s);
	key_up = key_down = false;
	last_ms = k_uptime_get_32();
	app_set_frame_ms(FRAME_MS);
	if (wg.state == ST_PLAY && !wg.paused) {
		music_start();
	}
}

static void witch_leave(struct ui_screen *s)
{
	ARG_UNUSED(s);
	app_set_frame_ms(0);
	music_stop();
	save_hi();
	if (wg.state == ST_PLAY) {
		wg.paused = true;
	}
}

struct ui_screen witch_screen = {
	.title = "Hexenflug",
	.enter = witch_enter,
	.leave = witch_leave,
	.event = witch_event,
	.draw = witch_draw,
	.poll = witch_poll,
};

void witch_open(void)
{
	if (settings_load_one(HI_SCORE_KEY, &wg.hi, sizeof(wg.hi)) != sizeof(wg.hi)) {
		wg.hi = 0;
	}
	wg.rnd = k_cycle_get_32() | 1;
	if (wg.state != ST_PLAY) {
		/* A paused flight is continued; otherwise the title. */
		memset(wg.things, 0, sizeof(wg.things));
		wg.state = ST_TITLE;
		wg.y = wg.target = ((TOP_Y + BOTTOM_Y - WITCH_H) / 2) * SUB;
		wg.lives = START_LIVES;
		wg.score = 0;
	}
	ui_push(&witch_screen);
}
