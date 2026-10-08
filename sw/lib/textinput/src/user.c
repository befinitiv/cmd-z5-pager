#include <string.h>

#include "textinput/user.h"

#define COUNT_MAX 60000

void ti_user_init(struct ti_user *u)
{
	memset(u, 0, sizeof(*u));
	u->magic = TI_USER_MAGIC;
}

bool ti_user_valid(const struct ti_user *u)
{
	return u->magic == TI_USER_MAGIC;
}

/* Halve all counts, so old habits fade and counters do not overflow. */
static void decay(struct ti_user *u)
{
	u->total = 0;
	for (int i = 0; i < TI_USER_WORDS; i++) {
		u->words[i].count /= 2;
		u->total += u->words[i].count;
	}
	for (int i = 0; i < TI_USER_BIGRAMS; i++) {
		u->bigrams[i].count /= 2;
	}
}

static int word_slot(const struct ti_user *u, int32_t id)
{
	if (id >= TI_USER_BASE) {
		int slot = id - TI_USER_BASE;

		return slot < TI_USER_WORDS && u->words[slot].count ? slot : -1;
	}
	for (int i = 0; i < TI_USER_WORDS; i++) {
		if (u->words[i].count && u->words[i].lex_id == id) {
			return i;
		}
	}
	return -1;
}

static int own_slot(const struct ti_user *u, const char *word)
{
	for (int i = 0; i < TI_USER_WORDS; i++) {
		if (u->words[i].count && u->words[i].lex_id < 0 &&
		    strcmp(u->words[i].text, word) == 0) {
			return i;
		}
	}
	return -1;
}

/* Entries are replaced by the least valuable: unused, else lowest count, then oldest. */
static bool worse(uint16_t c1, uint16_t s1, uint16_t c2, uint16_t s2)
{
	return c1 < c2 || (c1 == c2 && (int16_t)(s1 - s2) < 0);
}

static int word_victim(const struct ti_user *u)
{
	int best = 0;

	for (int i = 1; i < TI_USER_WORDS; i++) {
		if (worse(u->words[i].count, u->words[i].stamp, u->words[best].count,
			  u->words[best].stamp)) {
			best = i;
		}
	}
	return best;
}

static int bigram_victim(const struct ti_user *u)
{
	int best = 0;

	for (int i = 1; i < TI_USER_BIGRAMS; i++) {
		if (worse(u->bigrams[i].count, u->bigrams[i].stamp, u->bigrams[best].count,
			  u->bigrams[best].stamp)) {
			best = i;
		}
	}
	return best;
}

static void forget_pairs(struct ti_user *u, int32_t id)
{
	for (int i = 0; i < TI_USER_BIGRAMS; i++) {
		if (u->bigrams[i].w1 == id || u->bigrams[i].w2 == id) {
			u->bigrams[i].count = 0;
		}
	}
}

int32_t ti_user_find(const struct ti_user *u, const struct ti_model *m, const char *word)
{
	int32_t id = ti_word_find(m, word);

	if (id >= 0) {
		return id;
	}
	int slot = own_slot(u, word);

	return slot >= 0 ? TI_USER_BASE + slot : -1;
}

int32_t ti_user_learn(struct ti_user *u, const struct ti_model *m, int32_t prev, const char *word)
{
	int32_t lex = ti_word_find(m, word);
	int slot = lex >= 0 ? word_slot(u, lex) : own_slot(u, word);

	if (lex < 0 && strlen(word) >= TI_USER_TEXT) {
		return -1;
	}
	u->clock++;
	if (slot < 0) {
		slot = word_victim(u);
		if (u->words[slot].count) {
			u->total -= u->words[slot].count;
			forget_pairs(u, u->words[slot].lex_id >= 0 ? u->words[slot].lex_id
								    : TI_USER_BASE + slot);
		}
		memset(&u->words[slot], 0, sizeof(u->words[slot]));
		u->words[slot].lex_id = lex;
		if (lex < 0) {
			strcpy(u->words[slot].text, word);
		}
	}
	if (u->words[slot].count >= COUNT_MAX) {
		decay(u);
	}
	u->words[slot].count++;
	u->words[slot].stamp = u->clock;
	u->total++;

	int32_t id = lex >= 0 ? lex : TI_USER_BASE + slot;

	if (prev >= 0) {
		int b = -1;

		for (int i = 0; i < TI_USER_BIGRAMS; i++) {
			if (u->bigrams[i].count && u->bigrams[i].w1 == prev && u->bigrams[i].w2 == id) {
				b = i;
				break;
			}
		}
		if (b < 0) {
			b = bigram_victim(u);
			u->bigrams[b] = (struct ti_user_bigram){.w1 = prev, .w2 = id};
		}
		if (u->bigrams[b].count < COUNT_MAX) {
			u->bigrams[b].count++;
		}
		u->bigrams[b].stamp = u->clock;
	}
	return id;
}

uint16_t ti_user_count(const struct ti_user *u, int32_t id)
{
	int slot = word_slot(u, id);

	return slot >= 0 ? u->words[slot].count : 0;
}

uint16_t ti_user_pair(const struct ti_user *u, int32_t prev, int32_t id, uint32_t *prev_total)
{
	uint16_t c = 0;
	uint32_t total = 0;

	for (int i = 0; i < TI_USER_BIGRAMS; i++) {
		const struct ti_user_bigram *b = &u->bigrams[i];

		if (b->count && b->w1 == prev) {
			total += b->count;
			if (b->w2 == id) {
				c = b->count;
			}
		}
	}
	if (prev_total) {
		*prev_total = total;
	}
	return c;
}

int ti_user_successors(const struct ti_user *u, int32_t prev, int32_t *ids, int max)
{
	int n = 0;

	for (int i = 0; i < TI_USER_BIGRAMS && n < max; i++) {
		if (u->bigrams[i].count && u->bigrams[i].w1 == prev) {
			ids[n++] = u->bigrams[i].w2;
		}
	}
	return n;
}

int ti_user_prefix(const struct ti_user *u, const char *keys, int32_t *ids, bool *exact, int max)
{
	int n = 0;
	size_t nk = strlen(keys);

	for (int i = 0; i < TI_USER_WORDS && n < max; i++) {
		const struct ti_user_word *w = &u->words[i];

		if (!w->count || w->lex_id >= 0) {
			continue;
		}
		const char *p = w->text;
		size_t k = 0;
		bool match = true;

		while (*p && k < nk) {
			if (ti_key_of_char(ti_utf8_next(&p)) != keys[k++]) {
				match = false;
				break;
			}
		}
		if (match && k == nk) {
			exact[n] = *p == '\0';
			ids[n++] = TI_USER_BASE + i;
		}
	}
	return n;
}

const char *ti_user_text(const struct ti_user *u, int32_t id)
{
	int slot = id - TI_USER_BASE;

	if (id < TI_USER_BASE || slot >= TI_USER_WORDS || !u->words[slot].count) {
		return NULL;
	}
	return u->words[slot].text;
}
