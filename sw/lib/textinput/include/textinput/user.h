/*
 * User model: words and word pairs the user has typed, so predictions adapt
 * to the user. Words that are not in the lexicon (names, slang) are stored
 * with their spelling and become available in predictive mode.
 *
 * Fixed size, no dynamic memory. The whole struct can be stored as a blob
 * (ti_user_valid() checks a loaded blob).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "textinput/model.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TI_USER_WORDS   64
#define TI_USER_BIGRAMS 128
#define TI_USER_TEXT    24
/* Ids of user words are TI_USER_BASE + slot. */
#define TI_USER_BASE    0x100000
#define TI_USER_MAGIC   0x55534552u /* "USER" */

struct ti_user_word {
	int32_t lex_id;             /* lexicon word, or -1 for an own word */
	char text[TI_USER_TEXT];    /* own word: spelling (UTF-8) */
	uint16_t count;
	uint16_t stamp;
};

struct ti_user_bigram {
	int32_t w1, w2; /* lexicon ids, user ids or the sentence start (model V) */
	uint16_t count;
	uint16_t stamp;
};

struct ti_user {
	uint32_t magic;
	uint16_t clock;
	uint16_t reserved;
	uint32_t total; /* sum of word counts */
	struct ti_user_word words[TI_USER_WORDS];
	struct ti_user_bigram bigrams[TI_USER_BIGRAMS];
};

void ti_user_init(struct ti_user *u);
bool ti_user_valid(const struct ti_user *u);

/*
 * Record that `word` (UTF-8) was typed after `prev` (an id, or -1 if
 * unknown). Returns the word's id: its lexicon id, or a user id.
 */
int32_t ti_user_learn(struct ti_user *u, const struct ti_model *m, int32_t prev, const char *word);

/* Id of a word: lexicon first, then own words. -1 if unknown. */
int32_t ti_user_find(const struct ti_user *u, const struct ti_model *m, const char *word);

uint16_t ti_user_count(const struct ti_user *u, int32_t id);
/* Count of the pair and of all pairs starting with prev. */
uint16_t ti_user_pair(const struct ti_user *u, int32_t prev, int32_t id, uint32_t *prev_total);

/* Words that followed prev; returns how many ids were written. */
int ti_user_successors(const struct ti_user *u, int32_t prev, int32_t *ids, int max);
/* Own words whose key sequence starts with keys; exact: same length only. */
int ti_user_prefix(const struct ti_user *u, const char *keys, int32_t *ids, bool *exact, int max);

/* Text of an own word (id >= TI_USER_BASE), or NULL. */
const char *ti_user_text(const struct ti_user *u, int32_t id);

#ifdef __cplusplus
}
#endif
