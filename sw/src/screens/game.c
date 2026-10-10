/*
 * "Z5 Jump": a small jump and run in the style of the Game Boy platformers,
 * started with the wheel bent down on the home screen.
 *
 * Levels of 8 x 8 px tiles (tools/game/level.py), scrolling to the right
 * only. 1-1 is overground: walkers are defeated by jumping on them or by
 * bumping the block they stand on, the flag ends it. 1-2 is under water: the
 * jump keys swim, fish and jellies cannot be stomped, an exit pipe ends it.
 * The grow item from 'M' blocks absorbs one hit and lets the hero break
 * bricks. Lives, coins, a timer and the best score (stored in the settings).
 *
 * Keys: 4/7 left, 6/9 right, 2/5/8/0 or wheel push jump (held = higher),
 * 1/3 jump to the left/right, CALL pause, C or wheel bent up leaves.
 *
 * Positions are in 1/16 px; the game runs at 30 steps per second.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include "../app.h"
#include "../config.h"
#include "../hw.h"
#include "../power.h"
#include "game_level.h"
#include "screens.h"

/* The level the game starts with (index into levels[]). */
#define START_LEVEL 0

#define FRAME_MS 33
#define SUB      16 /* subpixels per pixel */
#define ROWS     8
#define PLAY_Y   8 /* playfield below the score line */

/* Physics, in subpixels per frame (squared). */
#define ACCEL        3
#define AIR_ACCEL    2
#define FRICTION     3
#define MAX_VX       24
#define GRAV         10
#define GRAV_HOLD    4 /* while rising with jump held: higher jumps */
#define MAX_VY       64
#define JUMP_V       58
#define STOMP_V      36
#define STOMP_HOLD_V 56
#define WALKER_V     8
#define ITEM_V       12
#define JUMP_BUFFER  4 /* frames a jump press is remembered before landing */

/* Under water: slow sinking, a stroke per jump key press. */
#define WATER_GRAV     2
#define WATER_MAX_VY   16
#define WATER_MAX_VX   16
#define WATER_ACCEL    2
#define WATER_FRICTION 1
#define SWIM_V         26
#define FISH_V         8
#define JELLY_BURST_V  22
#define JELLY_BURST_VX 10
#define JELLY_SINK_V   5

#define START_LIVES   3
#define START_TIME    300
#define TIME_FRAMES   12 /* frames per time unit */
#define HURT_FRAMES   60
#define INTRO_FRAMES  50
#define HI_SCORE_KEY  "game/hi"

#define MAX_WALKERS   32
#define MAX_ITEMS     2
#define MAX_PARTICLES 12

/* Sprites ------------------------------------------------------------------- */

/* '.' transparent, 'w' white, 'l' light, 'd' dark, 'b' black. 8 px wide. */
typedef const char *const art_t[];

static art_t hero_stand = {
	"..bbbb..", ".bbbbbbb", "..dlbll.", "..llllb.",
	"..bdbb..", ".lbbbbl.", "..bb.bb.", ".bbb.bbb",
};
static art_t hero_walk = {
	"..bbbb..", ".bbbbbbb", "..dlbll.", "..llllb.",
	"..bdbb..", "..lbbl..", "..bbbb..", "..b..bb.",
};
static art_t hero_jump = {
	"..bbbb.l", ".bbbbbbb", "..dlbll.", "..llllb.",
	".lbdbbb.", "..bbbb..", ".bb..bb.", "b.....bb",
};
static art_t big_stand = {
	"..bbbb..", ".bbbbbbb", "..dlbll.", ".dllllb.", "..llll..", ".bbdbb..", "bbbdbbbb",
	"lbdddbbl", "l.dddd.l", "..dddd..", ".dd..dd.", ".dd..dd.", ".bb..bb.", "bbb..bbb",
};
static art_t big_walk = {
	"..bbbb..", ".bbbbbbb", "..dlbll.", ".dllllb.", "..llll..", ".bbdbb..", ".bbdbbl.",
	".lddddl.", "..dddd..", "..dddd..", "..dddd..", "..ddd...", "..bbbb..", "..b.bbb.",
};
static art_t big_jump = {
	"..bbbb.l", ".bbbbbbl", "..dlbll.", ".dllllb.", "..llll..", ".bbdbbb.", "bbbdbb..",
	"lbdddb..", "l.dddd..", "..dddd..", ".dd..dd.", "dd....dd", "bb.....b", "b.......",
};
static art_t walker_a = {
	"..dddd..", ".dddddd.", "dwbddbwd", "dwbddbwd",
	"dddddddd", "..llll..", ".bllllb.", "bb....bb",
};
static art_t walker_b = {
	"..dddd..", ".dddddd.", "dwbddbwd", "dwbddbwd",
	"dddddddd", "..llll..", "..llll..", ".bb..bb.",
};
static art_t walker_flat = {
	"........", "........", "........", "........",
	"........", ".dddddd.", "dwbddbwd", "bbbbbbbb",
};
static art_t item_grow = {
	"..bbbb..", ".bddlldb", "bdlllldb", "bddddddb",
	"bbbbbbbb", ".blwwlb.", ".bllllb.", "..bbbb..",
};
static art_t tile_ground = {
	"bbbbbbbb", "dlddddld", "dddddddd", "ddlddddd",
	"dddddldd", "dddddddd", "dlddddld", "dddddddd",
};
static art_t tile_brick = {
	"bbbbbbbb", "dddbdddd", "dddbdddd", "bbbbbbbb",
	"ddddddbd", "ddddddbd", "bbbbbbbb", "dddbdddd",
};
static art_t tile_question = {
	"bbbbbbbb", "bllbbblb", "blblllbb", "bllllblb",
	"blllbllb", "bllllllb", "blllbllb", "bbbbbbbb",
};
static art_t tile_used = {
	"bbbbbbbb", "bddddddb", "bdbddbdb", "bddddddb",
	"bddddddb", "bdbddbdb", "bddddddb", "bbbbbbbb",
};
static art_t tile_hard = {
	"lllllllb", "llllllbb", "lldddbbb", "lldddbbb",
	"lldddbbb", "lldddbbb", "lbbbbbbb", "bbbbbbbb",
};
static art_t tile_pipe_top = {
	"bbbbbbbb", "bwlllddd", "bwlllddd", "bwlllddd",
	"bwlllddd", "bbbbbbbb", ".bwllddd", ".bwllddd",
};
static art_t tile_pipe_top_r = {
	"bbbbbbbb", "dddddddb", "dddddddb", "dddddddb",
	"dddddddb", "bbbbbbbb", "ddddddb.", "ddddddb.",
};
static art_t tile_pipe = {
	".bwllddd", ".bwllddd", ".bwllddd", ".bwllddd",
	".bwllddd", ".bwllddd", ".bwllddd", ".bwllddd",
};
static art_t tile_pipe_r = {
	"ddddddb.", "ddddddb.", "ddddddb.", "ddddddb.",
	"ddddddb.", "ddddddb.", "ddddddb.", "ddddddb.",
};
static art_t tile_coin = {
	"..bbbb..", ".bllllb.", ".blwdlb.", ".blwdlb.",
	".blwdlb.", ".blwdlb.", ".bllllb.", "..bbbb..",
};
static art_t tile_coin_thin = {
	"...bb...", "..bllb..", "..bwdb..", "..bwdb..",
	"..bwdb..", "..bwdb..", "..bllb..", "...bb...",
};
static art_t tile_pole = {
	"...bd...", "...bd...", "...bd...", "...bd...",
	"...bd...", "...bd...", "...bd...", "...bd...",
};
static art_t tile_pole_top = {
	"...bb...", "..bwlb..", "..bllb..", "...bb...",
	"...bd...", "...bd...", "...bd...", "...bd...",
};
static art_t flag_art = {
	"........", "....bbbb", "..bbdddb", "bbdddddb",
	"..bbdddb", "....bbbb", "........", "........",
};
static art_t cloud = {
	"........", ".....lll", "...lllll", ".lllllll",
	"llllllll", "llllllll", ".lllllll", "........",
};
static art_t bush = {
	"........", "........", "........", ".....lll",
	"...lllll", "..llllll", ".lllllll", "llllllll",
};
/* Under water. */
static art_t tile_sand = {
	"dddddddd", "ldllldll", "llllllld", "lldlllll",
	"lllllldl", "ldllllll", "llllldll", "dlllllll",
};
static art_t tile_coral = {
	"bdddbddb", "dlldddld", "ddddbddd", "dldddddl",
	"bddldddb", "dddddbdd", "ldbdddld", "dddddddd",
};
static art_t seaweed = {
	"...d....", "....d...", "...dl...", "..dl....",
	"...dl...", "....ld..", "...ld...", "...dd...",
};
static art_t exit_top = {
	"bbbbbbbb", "blllllll", "blbbbbbb", "blbbbbbb",
	"blbbbbbb", "blbbbbbb", "blbbbbbb", "blbbbbbb",
};
static art_t exit_bottom = {
	"blbbbbbb", "blbbbbbb", "blbbbbbb", "blbbbbbb",
	"blbbbbbb", "blbbbbbb", "blllllll", "bbbbbbbb",
};
static art_t pipe_side_top = {
	"bbbbbbbb", "llllllll", "dddddddd", "dddddddd",
	"dddddddd", "dddddddd", "dddddddd", "dddddddd",
};
static art_t pipe_side_bottom = {
	"dddddddd", "dddddddd", "dddddddd", "dddddddd",
	"dddddddd", "dddddddd", "dddddddd", "bbbbbbbb",
};
static art_t fish = { /* facing left */
	"........", "..bbbb.b", ".bdddddb", "bwbddddb",
	"bdddddbb", ".bbbbb.b", "........", "........",
};
static art_t jelly_sink = {
	"..bbbb..", ".bllllb.", "blwllwlb", "bllllllb",
	".bbbbbb.", "b.b..b.b", "b.b..b.b", ".b....b.",
};
static art_t jelly_burst = {
	"..bbbb..", ".bllllb.", "blwllwlb", "bllllllb",
	".bbbbbb.", "..b..b..", "..b..b..", "..b..b..",
};

static const enum gfx_color art_colors[] = {
	['w'] = GFX_WHITE, ['l'] = GFX_LIGHT, ['d'] = GFX_DARK, ['b'] = GFX_BLACK,
};

/* Draw w x h art at screen position x, y, clipped to the playfield. */
static void blit(struct gfx *g, int x, int y, const char *const *art, int h, bool flip_x,
		 bool flip_y)
{
	for (int r = 0; r < h; r++) {
		int sy = y + (flip_y ? h - 1 - r : r);

		if (sy < PLAY_Y || sy >= GFX_HEIGHT) {
			continue;
		}
		for (int c = 0; c < 8; c++) {
			char p = art[r][flip_x ? 7 - c : c];
			int sx = x + c;

			if (p != '.' && p != '\0' && sx >= 0 && sx < GFX_WIDTH) {
				gfx_pixel(g, sx, sy, art_colors[(uint8_t)p]);
			}
		}
	}
}

/* Sounds -------------------------------------------------------------------- */

static const struct hw_note snd_jump[] = {{392, 30}, {523, 30}, {659, 40}};
static const struct hw_note snd_coin[] = {{988, 70}, {1319, 200}};
static const struct hw_note snd_stomp[] = {{330, 40}, {196, 60}};
static const struct hw_note snd_bump[] = {{147, 60}};
static const struct hw_note snd_break[] = {{220, 30}, {110, 60}};
static const struct hw_note snd_grow[] = {{523, 60}, {659, 60}, {784, 60}, {1047, 60}, {1319, 120}};
static const struct hw_note snd_shrink[] = {{784, 60}, {523, 60}, {392, 60}, {262, 120}};
static const struct hw_note snd_life[] = {{1319, 80}, {1568, 80}, {2637, 80},
					  {2093, 80}, {2349, 80}, {3136, 120}};
static const struct hw_note snd_die[] = {{988, 100}, {0, 100}, {740, 120}, {622, 120}, {494, 300}};
static const struct hw_note snd_clear[] = {{523, 120}, {659, 120}, {784, 120},
					   {1047, 240}, {784, 120}, {1047, 360}};
static const struct hw_note snd_over[] = {{392, 200}, {330, 200}, {262, 400}};
static const struct hw_note snd_swim[] = {{330, 25}, {440, 35}};
static const struct hw_note snd_exit[] = {{196, 60}, {147, 60}, {110, 90}};

#define PLAY(snd) hw_melody(HW_SPEAKER, snd, ARRAY_SIZE(snd), app_config.volume)

/* State --------------------------------------------------------------------- */

enum state {
	ST_TITLE,
	ST_INTRO, /* "WORLD 1-x" before a level (and after a lost life) */
	ST_PLAY,
	ST_DYING,
	ST_CLEAR,
	ST_OVER,
};

enum {
	IN_LEFT = BIT(0),
	IN_RIGHT = BIT(1),
	IN_JUMP = BIT(2),
};

/* A moving box in subpixels. */
struct body {
	int32_t x, y;
	int16_t vx, vy;
	uint8_t w, h;
	bool ground;
};

enum walker_state {
	W_WAITING, /* not yet scrolled into view */
	W_ALIVE,
	W_FLAT,    /* stomped, shown flat for a moment */
	W_FALLING, /* knocked off by a bumped block */
	W_GONE,
};

enum walker_type {
	T_WALKER, /* 'g': walks, can be stomped */
	T_FISH,   /* 'f': swims straight ahead, bobbing */
	T_JELLY,  /* 'j': sinks, then pushes up towards the hero */
};

struct walker {
	struct body b;
	uint8_t type;
	uint8_t state;
	uint8_t timer;
};

struct item {
	struct body b;
	uint8_t emerge; /* frames left rising out of the block */
	bool on;
};

enum particle_kind {
	P_NONE,
	P_COIN,   /* coin popping out of a block */
	P_DEBRIS, /* piece of a broken brick */
	P_SCORE,  /* points floating up */
	P_BUBBLE, /* air bubble rising to the surface */
};

struct particle {
	uint8_t kind;
	uint8_t t;
	int16_t x, y, vx, vy; /* px; debris in subpixels */
	uint16_t value;
};

struct level {
	const char *rows[ROWS];
	int w;
	bool water;
	const char *name;
};

#define LEVEL_ROWS(m) {m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7]}

static const struct level levels[] = {
	{LEVEL_ROWS(level1_map), LEVEL1_W, false, "1-1"},
	{LEVEL_ROWS(level2_map), LEVEL2_W, true, "1-2"},
};

#define MAX_W MAX(LEVEL1_W, LEVEL2_W)

static struct {
	enum state state;
	bool paused;
	int level;
	bool water;
	int w; /* level width in tiles */
	char map[ROWS][MAX_W];
	int cam; /* px */
	struct body hero;
	bool big;
	bool facing_left;
	uint8_t hurt; /* invulnerable frames after a hit */
	uint8_t jump_buf;
	bool jump_held;
	uint8_t anim;
	struct walker walkers[MAX_WALKERS];
	int nwalkers;
	struct item items[MAX_ITEMS];
	struct particle parts[MAX_PARTICLES];
	int bump_tx, bump_ty, bump_t;
	int flag_tx; /* -1: no flag */
	int flag_y;  /* px from the playfield top */
	int exit_tx; /* -1: no exit pipe */
	bool clear_done;
	uint32_t score, hi;
	int coins, lives, time;
	int time_div;
	int timer; /* state timer in frames */
	uint32_t frame;
} gm;

static bool held[UI_KEY_COUNT];
static uint32_t last_ms;

/* Tiles and collisions ------------------------------------------------------ */

static char tile_at(int tx, int ty)
{
	if (tx < 0 || tx >= gm.w || ty < 0 || ty >= ROWS) {
		return ' ';
	}
	return gm.map[ty][tx];
}

static bool solid(char t)
{
	return t != ' ' && strchr("#B?MU=[]{}xy", t) != NULL;
}

enum {
	HIT_WALL = BIT(0),
	HIT_FLOOR = BIT(1),
	HIT_HEAD = BIT(2),
};

/* Move a body by its speed and push it out of solid tiles. */
static int move_body(struct body *b, bool collide, int *head_tx, int *head_ty)
{
	int res = 0;
	int l, r, t, bt;

	b->x += b->vx;
	l = b->x / SUB;
	r = l + b->w - 1;
	t = b->y >> 4;
	bt = t + b->h - 1;
	if (collide && b->vx) {
		int tx = (b->vx > 0 ? r : l) >> 3;

		for (int ty = t >> 3; ty <= bt >> 3; ty++) {
			if (solid(tile_at(tx, ty))) {
				b->x = (b->vx > 0 ? tx * 8 - b->w : (tx + 1) * 8) * SUB;
				res |= HIT_WALL;
				break;
			}
		}
	}

	b->y += b->vy;
	l = b->x / SUB;
	r = l + b->w - 1;
	t = b->y >> 4;
	bt = t + b->h - 1;
	b->ground = false;
	if (!collide) {
		return res;
	}
	if (b->vy > 0) {
		int ty = bt >> 3;

		for (int tx = l >> 3; tx <= r >> 3; tx++) {
			if (solid(tile_at(tx, ty))) {
				b->y = (ty * 8 - b->h) * SUB;
				b->vy = 0;
				b->ground = true;
				res |= HIT_FLOOR;
				break;
			}
		}
	} else if (b->vy < 0) {
		int ty = t >> 3;
		int cx = ((l + r) / 2) >> 3;
		int hit = -1;

		/* The block under the middle of the head wins. */
		if (solid(tile_at(cx, ty))) {
			hit = cx;
		} else if (solid(tile_at(l >> 3, ty))) {
			hit = l >> 3;
		} else if (solid(tile_at(r >> 3, ty))) {
			hit = r >> 3;
		}
		if (hit >= 0) {
			b->y = (ty + 1) * 8 * SUB;
			b->vy = 0;
			res |= HIT_HEAD;
			if (head_tx) {
				*head_tx = hit;
				*head_ty = ty;
			}
		}
	}
	return res;
}

static bool overlap(const struct body *a, const struct body *b, int shrink)
{
	int ax = a->x / SUB, ay = a->y >> 4, bx = b->x / SUB, by = b->y >> 4;

	return ax < bx + b->w - shrink && bx + shrink < ax + a->w && ay < by + b->h - shrink &&
	       by + shrink < ay + a->h;
}

/* Effects and scoring ------------------------------------------------------- */

static struct particle *new_particle(enum particle_kind kind)
{
	for (int i = 0; i < MAX_PARTICLES; i++) {
		if (gm.parts[i].kind == P_NONE) {
			memset(&gm.parts[i], 0, sizeof(gm.parts[i]));
			gm.parts[i].kind = kind;
			return &gm.parts[i];
		}
	}
	return NULL;
}

static void add_score(uint32_t points, int x, int y)
{
	struct particle *p = new_particle(P_SCORE);

	gm.score += points;
	if (p) {
		p->x = x;
		p->y = y;
		p->value = points;
	}
}

static void add_coin(void)
{
	gm.score += 200;
	if (++gm.coins >= 100) {
		gm.coins = 0;
		gm.lives++;
		PLAY(snd_life);
	} else {
		PLAY(snd_coin);
	}
}

/* Level --------------------------------------------------------------------- */

static void load_level(void)
{
	memset(gm.walkers, 0, sizeof(gm.walkers));
	memset(gm.items, 0, sizeof(gm.items));
	memset(gm.parts, 0, sizeof(gm.parts));
	static const char spawns[] = "gfj"; /* in the order of enum walker_type */
	const struct level *lv = &levels[gm.level];

	gm.nwalkers = 0;
	gm.w = lv->w;
	gm.water = lv->water;
	gm.flag_tx = -1;
	gm.exit_tx = -1;
	gm.clear_done = false;
	for (int y = 0; y < ROWS; y++) {
		memcpy(gm.map[y], lv->rows[y], gm.w);
		for (int x = 0; x < gm.w; x++) {
			char t = gm.map[y][x];
			const char *spawn = strchr(spawns, t);

			if (t != ' ' && spawn && gm.nwalkers < MAX_WALKERS) {
				struct walker *w = &gm.walkers[gm.nwalkers];

				w->type = spawn - spawns;
				w->b = (struct body){.x = x * 8 * SUB, .y = y * 8 * SUB, .w = 8, .h = 8};
				/* Fish alternate between slow and a little faster. */
				w->b.vx = w->type == T_WALKER ? -WALKER_V
					  : w->type == T_FISH  ? -FISH_V - (gm.nwalkers % 2) * 4
							       : 0;
				w->state = W_WAITING;
				gm.nwalkers++;
				gm.map[y][x] = ' ';
			} else if (t == 'F') {
				gm.flag_tx = x;
			} else if (t == 'X' && gm.exit_tx < 0) {
				gm.exit_tx = x;
			}
		}
	}
	gm.hero = (struct body){.x = 16 * SUB, .y = 40 * SUB, .w = 6, .h = 8};
	gm.big = false;
	gm.facing_left = false;
	gm.hurt = 0;
	gm.jump_buf = 0;
	gm.cam = 0;
	gm.bump_t = 0;
	gm.flag_y = 8;
	gm.time = START_TIME;
	gm.time_div = 0;
}

static void save_hi(void)
{
	if (gm.score > gm.hi) {
		gm.hi = gm.score;
		settings_save_one(HI_SCORE_KEY, &gm.hi, sizeof(gm.hi));
	}
}

static void start_level(int level)
{
	gm.level = level;
	gm.paused = false;
	load_level();
	gm.state = ST_INTRO;
	gm.timer = 0;
}

static void new_game(void)
{
	gm.score = 0;
	gm.coins = 0;
	gm.lives = START_LIVES;
	start_level(START_LEVEL);
}

static void set_big(bool big)
{
	if (big == gm.big) {
		return;
	}
	gm.big = big;
	/* Keep the feet where they are. */
	gm.hero.y += (big ? -6 : 6) * SUB;
	gm.hero.h = big ? 14 : 8;
}

static void die(void)
{
	gm.state = ST_DYING;
	gm.timer = 0;
	gm.hero.vx = 0;
	gm.hero.vy = 0;
	PLAY(snd_die);
}

static void hurt(void)
{
	if (gm.hurt) {
		return;
	}
	if (gm.big) {
		set_big(false);
		gm.hurt = HURT_FRAMES;
		PLAY(snd_shrink);
	} else {
		die();
	}
}

/* A block was hit from below. */
static void bump_block(int tx, int ty)
{
	char *t = &gm.map[ty][tx];

	if (*t == '?' || *t == 'M') {
		if (*t == '?') {
			struct particle *p = new_particle(P_COIN);

			if (p) {
				p->x = tx * 8;
				p->y = ty * 8 - 8;
			}
			add_coin();
		} else {
			for (int i = 0; i < MAX_ITEMS; i++) {
				struct item *it = &gm.items[i];

				if (!it->on) {
					it->on = true;
					it->emerge = 8;
					it->b = (struct body){.x = tx * 8 * SUB, .y = ty * 8 * SUB,
							      .vx = ITEM_V, .w = 8, .h = 8};
					break;
				}
			}
			PLAY(snd_bump);
		}
		*t = 'U';
	} else if (*t == 'B' && gm.big) {
		*t = ' ';
		for (int i = 0; i < 4; i++) {
			struct particle *p = new_particle(P_DEBRIS);

			if (p) {
				p->x = (tx * 8 + (i & 1) * 4) * SUB;
				p->y = (ty * 8 + (i >> 1) * 4) * SUB;
				p->vx = (i & 1) ? 12 : -12;
				p->vy = (i >> 1) ? -40 : -56;
			}
		}
		gm.score += 50;
		PLAY(snd_break);
		return;
	} else {
		PLAY(snd_bump);
	}
	gm.bump_tx = tx;
	gm.bump_ty = ty;
	gm.bump_t = 6;

	/* Walkers standing on the block are knocked off. */
	for (int i = 0; i < gm.nwalkers; i++) {
		struct walker *w = &gm.walkers[i];
		int wx = w->b.x / SUB, wy = (w->b.y >> 4) + w->b.h;

		if (w->state == W_ALIVE && wy == ty * 8 && wx + 8 > tx * 8 && wx < tx * 8 + 8) {
			w->state = W_FALLING;
			w->b.vy = -40;
			add_score(100, wx, wy - 16);
		}
	}
}

/* Update -------------------------------------------------------------------- */

static uint8_t input(void)
{
	static const uint8_t bits[UI_KEY_COUNT] = {
		[UI_KEY_4] = IN_LEFT,  [UI_KEY_7] = IN_LEFT,  [UI_KEY_6] = IN_RIGHT,
		[UI_KEY_9] = IN_RIGHT, [UI_KEY_2] = IN_JUMP,  [UI_KEY_5] = IN_JUMP,
		[UI_KEY_8] = IN_JUMP,  [UI_KEY_0] = IN_JUMP,  [UI_KEY_SELECT] = IN_JUMP,
		[UI_KEY_1] = IN_LEFT | IN_JUMP, [UI_KEY_3] = IN_RIGHT | IN_JUMP,
	};
	uint8_t in = 0;

	for (int k = 0; k < UI_KEY_COUNT; k++) {
		if (held[k]) {
			in |= bits[k];
		}
	}
	return in;
}

static bool is_jump_key(enum ui_key k)
{
	return k == UI_KEY_1 || k == UI_KEY_2 || k == UI_KEY_3 || k == UI_KEY_5 ||
	       k == UI_KEY_8 || k == UI_KEY_0 || k == UI_KEY_SELECT;
}

static void update_hero(void)
{
	struct body *h = &gm.hero;
	uint8_t in = input();
	int dir = ((in & IN_RIGHT) ? 1 : 0) - ((in & IN_LEFT) ? 1 : 0);
	int accel = gm.water ? WATER_ACCEL : h->ground ? ACCEL : AIR_ACCEL;
	int max_vx = gm.water ? WATER_MAX_VX : MAX_VX;
	int friction = gm.water ? WATER_FRICTION : FRICTION;
	int head_tx, head_ty;

	gm.jump_held = in & IN_JUMP;
	if (dir) {
		h->vx = CLAMP(h->vx + dir * accel, -max_vx, max_vx);
		gm.facing_left = dir < 0;
	} else if (h->ground || gm.water) {
		h->vx = h->vx > 0 ? MAX(h->vx - friction, 0) : MIN(h->vx + friction, 0);
	}

	if (gm.water) {
		/* Every press is a stroke, also while swimming. */
		if (gm.jump_buf) {
			gm.jump_buf = 0;
			h->vy = -SWIM_V;
			PLAY(snd_swim);
		}
		h->vy = MIN(h->vy + WATER_GRAV, WATER_MAX_VY);
	} else {
		if (gm.jump_buf && h->ground) {
			gm.jump_buf = 0;
			/* Running jumps go a little higher. */
			h->vy = -(JUMP_V + abs(h->vx) / 4);
			PLAY(snd_jump);
		}
		if (gm.jump_buf) {
			gm.jump_buf--;
		}
		h->vy = MIN(h->vy + ((h->vy < 0 && gm.jump_held) ? GRAV_HOLD : GRAV), MAX_VY);
	}

	int res = move_body(h, true, &head_tx, &head_ty);

	if (res & HIT_HEAD) {
		bump_block(head_tx, head_ty);
	}
	if (gm.water) {
		/* The hero stays below the surface and breathes out now and then. */
		if (h->y < 0) {
			h->y = 0;
			h->vy = MAX(h->vy, 0);
		}
		if (gm.frame % 40 == 0) {
			struct particle *p = new_particle(P_BUBBLE);

			if (p) {
				p->x = h->x / SUB + (gm.facing_left ? 0 : h->w - 1);
				p->y = (h->y >> 4) + 1;
			}
		}
	}
	/* The camera only moves forward; the hero cannot walk back out of view. */
	if (h->x < gm.cam * SUB) {
		h->x = gm.cam * SUB;
		h->vx = 0;
	}
	gm.cam = CLAMP(MAX(gm.cam, h->x / SUB - 40), 0, gm.w * 8 - GFX_WIDTH);

	/* Coins. */
	int l = h->x / SUB, t = h->y >> 4;

	for (int ty = t >> 3; ty <= (t + h->h - 1) >> 3; ty++) {
		for (int tx = l >> 3; tx <= (l + h->w - 1) >> 3; tx++) {
			char t = tile_at(tx, ty);

			if (t == 'o') {
				gm.map[ty][tx] = ' ';
				add_coin();
			} else if ((t == 'X' || t == 'Y') && gm.state == ST_PLAY) {
				/* Into the exit pipe. */
				gm.state = ST_CLEAR;
				gm.timer = 0;
				h->vx = 0;
				h->vy = 0;
				PLAY(snd_exit);
			}
		}
	}

	if (t > ROWS * 8) {
		die();
		return;
	}
	if (gm.state == ST_CLEAR) {
		return;
	}
	if (gm.flag_tx >= 0 && l + h->w / 2 >= gm.flag_tx * 8 + 2) {
		gm.state = ST_CLEAR;
		gm.timer = 0;
		h->x = (gm.flag_tx * 8 - 2) * SUB;
		h->vx = 0;
		h->vy = 0;
		add_score(MAX(100, (64 - t) / 8 * 400), l, t);
		PLAY(snd_clear);
		return;
	}
	if (dir || !h->ground) {
		gm.anim++;
	}
	if (gm.hurt) {
		gm.hurt--;
	}
}

/* Jelly: sinks while the hero is below it, else pushes up towards the hero. */
static void update_jelly(struct walker *w)
{
	if (w->timer) {
		w->timer--;
	} else if ((gm.hero.y >> 4) + gm.hero.h / 2 < (w->b.y >> 4) + 8) {
		w->b.vy = -JELLY_BURST_V;
		w->b.vx = gm.hero.x > w->b.x ? JELLY_BURST_VX : -JELLY_BURST_VX;
		w->timer = 20;
	} else {
		w->b.vx = 0;
		w->timer = 10;
	}
	/* Water drag: bursts slow down into sinking. */
	if (w->b.vy < JELLY_SINK_V) {
		w->b.vy += 2;
	}
}

static void update_walkers(void)
{
	for (int i = 0; i < gm.nwalkers; i++) {
		struct walker *w = &gm.walkers[i];
		int wx = w->b.x / SUB;

		switch (w->state) {
		case W_WAITING:
			if (wx < gm.cam + GFX_WIDTH + 8) {
				w->state = W_ALIVE;
			}
			break;
		case W_ALIVE:
			if (w->type == T_FISH) {
				w->b.vy = 0;
			} else if (w->type == T_JELLY) {
				update_jelly(w);
			} else {
				w->b.vy = MIN(w->b.vy + GRAV, MAX_VY);
			}
			if (move_body(&w->b, true, NULL, NULL) & HIT_WALL) {
				w->b.vx = -w->b.vx;
			}
			if (w->b.y < 0) {
				w->b.y = 0;
			}
			if ((w->b.y >> 4) > ROWS * 8 || wx < gm.cam - 16) {
				w->state = W_GONE;
				break;
			}
			if (gm.state != ST_PLAY || !overlap(&gm.hero, &w->b, 1)) {
				break;
			}
			if (w->type != T_WALKER) {
				/* Swimmers cannot be stomped. */
				hurt();
				break;
			}
			if (gm.hero.vy > 0 && (gm.hero.y >> 4) + gm.hero.h <= (w->b.y >> 4) + 5) {
				w->state = W_FLAT;
				w->timer = 15;
				gm.hero.vy = gm.jump_held ? -STOMP_HOLD_V : -STOMP_V;
				add_score(100, wx, (w->b.y >> 4) - 8);
				PLAY(snd_stomp);
			} else {
				hurt();
			}
			break;
		case W_FLAT:
			if (--w->timer == 0) {
				w->state = W_GONE;
			}
			break;
		case W_FALLING:
			w->b.vy = MIN(w->b.vy + GRAV, MAX_VY);
			move_body(&w->b, false, NULL, NULL);
			if ((w->b.y >> 4) > ROWS * 8 + 16) {
				w->state = W_GONE;
			}
			break;
		default:
			break;
		}
	}
}

static void update_items(void)
{
	for (int i = 0; i < MAX_ITEMS; i++) {
		struct item *it = &gm.items[i];

		if (!it->on) {
			continue;
		}
		if (it->emerge) {
			it->emerge--;
			it->b.y -= SUB;
			continue;
		}
		it->b.vy = gm.water ? MIN(it->b.vy + WATER_GRAV, WATER_MAX_VY / 2)
				    : MIN(it->b.vy + GRAV, MAX_VY);
		if (move_body(&it->b, true, NULL, NULL) & HIT_WALL) {
			it->b.vx = -it->b.vx;
		}
		if ((it->b.y >> 4) > ROWS * 8 || it->b.x / SUB < gm.cam - 16) {
			it->on = false;
		} else if (gm.state == ST_PLAY && overlap(&gm.hero, &it->b, 0)) {
			it->on = false;
			add_score(1000, it->b.x / SUB, (it->b.y >> 4) - 8);
			if (!gm.big) {
				set_big(true);
			}
			PLAY(snd_grow);
		}
	}
}

static void update_particles(void)
{
	for (int i = 0; i < MAX_PARTICLES; i++) {
		struct particle *p = &gm.parts[i];

		switch (p->kind) {
		case P_COIN:
			p->y -= p->t < 6 ? 3 : -2;
			if (++p->t > 12) {
				p->kind = P_NONE;
			}
			break;
		case P_DEBRIS:
			p->vy += GRAV;
			p->x += p->vx;
			p->y += p->vy;
			if (++p->t > 40) {
				p->kind = P_NONE;
			}
			break;
		case P_SCORE:
			if (p->t % 2 == 0) {
				p->y--;
			}
			if (++p->t > 30) {
				p->kind = P_NONE;
			}
			break;
		case P_BUBBLE:
			if (p->t % 2 == 0) {
				p->y--;
			}
			if (solid(tile_at(p->x >> 3, p->y >> 3)) || p->y <= 1 || ++p->t > 90) {
				p->kind = P_NONE;
			}
			break;
		default:
			break;
		}
	}
	if (gm.bump_t) {
		gm.bump_t--;
	}
}

static void step(void)
{
	gm.frame++;
	switch (gm.state) {
	case ST_INTRO:
		if (++gm.timer >= INTRO_FRAMES) {
			gm.state = ST_PLAY;
		}
		break;
	case ST_PLAY:
		update_hero();
		if (gm.state == ST_PLAY && ++gm.time_div >= TIME_FRAMES) {
			gm.time_div = 0;
			if (--gm.time <= 0) {
				die();
			}
		}
		update_walkers();
		update_items();
		update_particles();
		break;
	case ST_DYING:
		/* Pause, then the hero jumps up and falls out of the picture. */
		if (++gm.timer == 15) {
			gm.hero.vy = -56;
		} else if (gm.timer > 15) {
			gm.hero.vy = MIN(gm.hero.vy + GRAV, MAX_VY);
			move_body(&gm.hero, false, NULL, NULL);
		}
		if (gm.timer >= 80) {
			if (--gm.lives > 0) {
				start_level(gm.level);
			} else {
				gm.state = ST_OVER;
				gm.timer = 0;
				save_hi();
				PLAY(snd_over);
			}
		}
		break;
	case ST_CLEAR: {
		bool arrived;

		gm.timer++;
		if (gm.water) {
			/* The hero swims into the exit pipe. */
			gm.hero.x += SUB;
			arrived = gm.hero.x / SUB >= gm.exit_tx * 8 + 4;
		} else {
			/* The flag and the hero slide down the pole. */
			if (gm.flag_y < 32) {
				gm.flag_y += 2;
			}
			if (!gm.hero.ground) {
				gm.hero.vy = 2 * SUB;
				move_body(&gm.hero, true, NULL, NULL);
			}
			arrived = gm.hero.ground;
		}
		/* Then the remaining time counts into the score. */
		if (arrived && gm.time > 0) {
			int n = MIN(gm.time, 3);

			gm.time -= n;
			gm.score += n * 50;
		} else if (arrived && !gm.clear_done) {
			gm.clear_done = true;
			save_hi();
		}
		update_particles();
		break;
	}
	case ST_OVER:
		gm.timer++;
		break;
	default:
		break;
	}
}

/* Drawing ------------------------------------------------------------------- */

static void draw_tile(struct gfx *g, char t, int tx, int ty, int sx, int sy)
{
	switch (t) {
	case '#':
		blit(g, sx, sy, gm.water ? tile_sand : tile_ground, 8, false, false);
		if (tile_at(tx, ty - 1) == '#') {
			/* Below the surface: no top edge. */
			gfx_hline(g, sx, sy, 8, gm.water ? GFX_LIGHT : GFX_DARK);
		}
		break;
	case 'B':
		blit(g, sx, sy, tile_brick, 8, false, false);
		break;
	case '?':
	case 'M':
		blit(g, sx, sy, tile_question, 8, false, false);
		break;
	case 'U':
		blit(g, sx, sy, tile_used, 8, false, false);
		break;
	case '=':
		blit(g, sx, sy, gm.water ? tile_coral : tile_hard, 8, false, false);
		break;
	case 'w':
		/* Seaweed sways; neighbours out of step. */
		blit(g, sx, sy, seaweed, 8, ((gm.frame / 20) + tx) % 2, false);
		break;
	case 'X':
		blit(g, sx, sy, exit_top, 8, false, false);
		break;
	case 'Y':
		blit(g, sx, sy, exit_bottom, 8, false, false);
		break;
	case 'x':
		blit(g, sx, sy, pipe_side_top, 8, false, false);
		break;
	case 'y':
		blit(g, sx, sy, pipe_side_bottom, 8, false, false);
		break;
	case '[':
		blit(g, sx, sy, tile_pipe_top, 8, false, false);
		break;
	case ']':
		blit(g, sx, sy, tile_pipe_top_r, 8, false, false);
		break;
	case '{':
		blit(g, sx, sy, tile_pipe, 8, false, false);
		break;
	case '}':
		blit(g, sx, sy, tile_pipe_r, 8, false, false);
		break;
	case 'o':
		blit(g, sx, sy, (gm.frame / 8) % 4 == 3 ? tile_coin_thin : tile_coin, 8, false,
		     false);
		break;
	case '|':
		blit(g, sx, sy, tile_pole, 8, false, false);
		break;
	case 'F':
		blit(g, sx, sy, tile_pole_top, 8, false, false);
		break;
	case '(':
		blit(g, sx, sy, cloud, 8, false, false);
		break;
	case ')':
		blit(g, sx, sy, cloud, 8, true, false);
		break;
	case '<':
		blit(g, sx, sy, bush, 8, false, false);
		break;
	case '>':
		blit(g, sx, sy, bush, 8, true, false);
		break;
	default:
		break;
	}
}

static void draw_world(struct gfx *g)
{
	int first = gm.cam / 8;

	if (gm.water) {
		/* Waves on the surface, drifting slowly. */
		for (int x = 0; x < GFX_WIDTH; x++) {
			int phase = (x + gm.cam + gm.frame / 4) % 12;

			gfx_pixel(g, x, PLAY_Y + (phase < 6 ? 0 : 1), GFX_LIGHT);
		}
	}
	for (int ty = 0; ty < ROWS; ty++) {
		for (int tx = first; tx <= first + GFX_WIDTH / 8 && tx < gm.w; tx++) {
			int sy = PLAY_Y + ty * 8;

			if (gm.bump_t && tx == gm.bump_tx && ty == gm.bump_ty) {
				sy -= gm.bump_t > 3 ? 6 - gm.bump_t : gm.bump_t;
			}
			draw_tile(g, gm.map[ty][tx], tx, ty, tx * 8 - gm.cam, sy);
		}
	}
	if (gm.flag_tx >= 0) {
		blit(g, gm.flag_tx * 8 - 5 - gm.cam, PLAY_Y + gm.flag_y, flag_art, 8, false, false);
	}
}

static void draw_sprites(struct gfx *g)
{
	char buf[8];

	for (int i = 0; i < MAX_ITEMS; i++) {
		const struct item *it = &gm.items[i];

		if (it->on) {
			/* While emerging, only the part above the block shows. */
			int h = it->emerge ? 8 - it->emerge : 8;

			blit(g, it->b.x / SUB - gm.cam, PLAY_Y + (it->b.y >> 4), item_grow, h, false,
			     false);
		}
	}
	for (int i = 0; i < gm.nwalkers; i++) {
		const struct walker *w = &gm.walkers[i];
		int sx = w->b.x / SUB - gm.cam, sy = PLAY_Y + (w->b.y >> 4);

		if (w->state == W_ALIVE && w->type == T_FISH) {
			/* Bobbing a pixel up and down. */
			static const int8_t bob[] = {0, 1, 2, 1};

			blit(g, sx, sy + bob[(gm.frame / 8 + i) % 4], fish, 8, w->b.vx > 0, false);
		} else if (w->state == W_ALIVE && w->type == T_JELLY) {
			blit(g, sx, sy, w->b.vy < 0 ? jelly_burst : jelly_sink, 8, false, false);
		} else if (w->state == W_ALIVE) {
			blit(g, sx, sy, (gm.frame / 6) % 2 ? walker_a : walker_b, 8, false, false);
		} else if (w->state == W_FLAT) {
			blit(g, sx, sy, walker_flat, 8, false, false);
		} else if (w->state == W_FALLING) {
			blit(g, sx, sy, walker_a, 8, false, true);
		}
	}
	for (int i = 0; i < MAX_PARTICLES; i++) {
		const struct particle *p = &gm.parts[i];

		switch (p->kind) {
		case P_COIN:
			blit(g, p->x - gm.cam, PLAY_Y + p->y, tile_coin_thin, 8, false, false);
			break;
		case P_DEBRIS:
			gfx_fill_rect(g, p->x / SUB - gm.cam, PLAY_Y + p->y / SUB, 3, 3, GFX_BLACK);
			break;
		case P_SCORE:
			snprintf(buf, sizeof(buf), "%u", p->value);
			gfx_text(g, gfx_font_small, p->x - gm.cam, PLAY_Y + p->y, buf, GFX_BLACK);
			break;
		case P_BUBBLE: {
			int bx = p->x - gm.cam + (p->t / 6) % 2;

			if (p->y >= 1) {
				gfx_rect(g, bx, PLAY_Y + p->y, 3, 3, GFX_DARK);
			}
			break;
		}
		default:
			break;
		}
	}

	/* The hero blinks while invulnerable and is gone once in the exit pipe. */
	bool in_pipe = gm.state == ST_CLEAR && gm.water && gm.hero.x / SUB >= gm.exit_tx * 8;

	if (gm.state != ST_TITLE && !in_pipe && !(gm.hurt && (gm.frame & 2))) {
		const struct body *h = &gm.hero;
		bool walking = h->vx != 0 && (gm.anim / 4) % 2;
		/* Swimming: arms and legs alternate. */
		bool stroke = gm.water && !h->ground && (gm.anim / 6) % 2;
		const char *const *art;

		if (gm.big) {
			art = stroke ? big_walk : !h->ground ? big_jump : walking ? big_walk : big_stand;
		} else {
			art = stroke                                 ? hero_walk
			      : !h->ground || gm.state == ST_DYING ? hero_jump
			      : walking                            ? hero_walk
								   : hero_stand;
		}
		blit(g, h->x / SUB - 1 - gm.cam, PLAY_Y + (h->y >> 4), art, h->h, gm.facing_left,
		     false);
	}
}

static void draw_hud(struct gfx *g)
{
	char buf[16];

	snprintf(buf, sizeof(buf), "%06u", (unsigned int)gm.score);
	gfx_text(g, gfx_font_small, 1, 1, buf, GFX_BLACK);
	/* Coin and heart icons, drawn with pixels. */
	gfx_fill_rect(g, 30, 1, 3, 5, GFX_DARK);
	gfx_vline(g, 31, 2, 3, GFX_LIGHT);
	snprintf(buf, sizeof(buf), "x%02d", gm.coins);
	gfx_text(g, gfx_font_small, 34, 1, buf, GFX_BLACK);
	gfx_hline(g, 52, 2, 2, GFX_BLACK);
	gfx_hline(g, 55, 2, 2, GFX_BLACK);
	gfx_hline(g, 51, 3, 7, GFX_BLACK);
	gfx_hline(g, 52, 4, 5, GFX_BLACK);
	gfx_hline(g, 53, 5, 3, GFX_BLACK);
	gfx_pixel(g, 54, 6, GFX_BLACK);
	snprintf(buf, sizeof(buf), "%d", MAX(gm.lives, 0));
	gfx_text(g, gfx_font_small, 59, 1, buf, GFX_BLACK);
	gfx_text(g, gfx_font_small, 67, 1, levels[gm.level].name, GFX_DARK);
	snprintf(buf, sizeof(buf), "%03d", MAX(gm.time, 0));
	gfx_text_aligned(g, gfx_font_small, 0, 1, GFX_WIDTH - 1, GFX_ALIGN_RIGHT, buf,
			 gm.time < 100 && (gm.frame & 8) ? GFX_DARK : GFX_BLACK);
	gfx_hline(g, 0, PLAY_Y - 1, GFX_WIDTH, GFX_DARK);
}

/* A framed box with centred lines of text. */
static void draw_box(struct gfx *g, int y, int h, const char *const *lines, int n)
{
	gfx_fill_rect(g, 8, y, GFX_WIDTH - 16, h, GFX_WHITE);
	gfx_rect(g, 8, y, GFX_WIDTH - 16, h, GFX_BLACK);
	for (int i = 0; i < n; i++) {
		gfx_text_aligned(g, i == 0 ? gfx_font_text : gfx_font_small, 8, y + 3 + i * 8 +
				 (i ? 1 : 0), GFX_WIDTH - 16, GFX_ALIGN_CENTER, lines[i], GFX_BLACK);
	}
}

static void game_draw(struct ui_screen *s, struct gfx *g)
{
	char line[3][20];

	ARG_UNUSED(s);
	draw_world(g);
	draw_sprites(g);
	draw_hud(g);

	switch (gm.state) {
	case ST_TITLE: {
		snprintf(line[0], sizeof(line[0]), "HI %06u", (unsigned int)gm.hi);
		const char *const l[] = {"Z5 JUMP", "push: start", line[0], "4/6 run  5 jump"};

		draw_box(g, 14, 40, l, ARRAY_SIZE(l));
		break;
	}
	case ST_OVER: {
		snprintf(line[0], sizeof(line[0]), "HI %06u", (unsigned int)gm.hi);
		const char *const l[] = {"GAME OVER", line[0], "push: again"};

		draw_box(g, 20, 30, l, ARRAY_SIZE(l));
		break;
	}
	case ST_INTRO: {
		snprintf(line[0], sizeof(line[0]), "WORLD %s", levels[gm.level].name);
		snprintf(line[1], sizeof(line[1]), "lives x%d", gm.lives);
		const char *const l[] = {line[0], line[1]};

		draw_box(g, 24, 22, l, ARRAY_SIZE(l));
		break;
	}
	case ST_CLEAR:
		if (gm.clear_done) {
			snprintf(line[0], sizeof(line[0]), "HI %06u", (unsigned int)MAX(gm.hi, gm.score));
			const char *const l[] = {"COURSE CLEAR!", line[0], "push: continue"};

			draw_box(g, 20, 30, l, ARRAY_SIZE(l));
		}
		break;
	default:
		if (gm.paused) {
			const char *const l[] = {"PAUSE", "CALL: continue"};

			draw_box(g, 24, 22, l, ARRAY_SIZE(l));
		}
		break;
	}
}

/* Screen -------------------------------------------------------------------- */

static bool game_event(struct ui_screen *s, const struct ui_event *ev)
{
	ARG_UNUSED(s);

	if (ev->type == UI_EV_WHEEL) {
		return true;
	}
	if (ev->type != UI_EV_KEY_PRESS && ev->type != UI_EV_KEY_RELEASE &&
	    ev->type != UI_EV_KEY_LONG) {
		return false;
	}
	/* C and wheel up leave the game (default handling). */
	if (ev->key == UI_KEY_CLEAR || ev->key == UI_KEY_BACK) {
		return false;
	}
	if (ev->type == UI_EV_KEY_RELEASE) {
		held[ev->key] = false;
		return true;
	}
	if (ev->type != UI_EV_KEY_PRESS) {
		return true;
	}
	held[ev->key] = true;

	switch (gm.state) {
	case ST_TITLE:
		if (is_jump_key(ev->key)) {
			new_game();
		}
		break;
	case ST_OVER:
		if (is_jump_key(ev->key) && gm.timer > 30) {
			save_hi();
			new_game();
		}
		break;
	case ST_CLEAR:
		/* On to the next level; after the last one back to the first. */
		if (is_jump_key(ev->key) && gm.clear_done) {
			start_level((gm.level + 1) % ARRAY_SIZE(levels));
		}
		break;
	case ST_INTRO:
		break;
	default:
		if (ev->key == UI_KEY_CALL) {
			gm.paused = !gm.paused;
		} else if (is_jump_key(ev->key) && !gm.paused) {
			gm.jump_buf = JUMP_BUFFER;
		}
		break;
	}
	ui_invalidate();
	return true;
}

static void game_poll(struct ui_screen *s)
{
	uint32_t now = k_uptime_get_32();
	int steps = 0;

	ARG_UNUSED(s);
	if (!power_display_on() || gm.paused) {
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
		if (gm.state == ST_PLAY && gm.frame % 30 == 0) {
			power_user_activity(false);
		}
	}
}

static void game_enter(struct ui_screen *s)
{
	ARG_UNUSED(s);
	memset(held, 0, sizeof(held));
	last_ms = k_uptime_get_32();
	app_set_frame_ms(FRAME_MS);
}

static void game_leave(struct ui_screen *s)
{
	ARG_UNUSED(s);
	app_set_frame_ms(0);
	save_hi();
	if (gm.state == ST_PLAY) {
		gm.paused = true;
	}
}

struct ui_screen game_screen = {
	.title = "Z5 Jump",
	.enter = game_enter,
	.leave = game_leave,
	.event = game_event,
	.draw = game_draw,
	.poll = game_poll,
};

void game_open(void)
{
	if (settings_load_one(HI_SCORE_KEY, &gm.hi, sizeof(gm.hi)) != sizeof(gm.hi)) {
		gm.hi = 0;
	}
	gm.level = START_LEVEL;
	load_level();
	gm.state = ST_TITLE;
	gm.frame = 0;
	gm.score = 0;
	gm.coins = 0;
	gm.lives = START_LIVES;
	ui_push(&game_screen);
}
