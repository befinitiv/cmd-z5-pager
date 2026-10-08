/*
 * Prediction engine: ranks words for a key sequence (T9 with completions)
 * or, with no keys, predicts the next word.
 *
 * Score of a word w after the words p2 p1:
 *   P(w) = 0.82 P_lm(w | p2 p1) + 0.08 P_user(w | p1) + 0.05 P_user(w)
 *          + 0.05 P_cache(w)
 * P_lm is the trigram/bigram/unigram backoff model; P_user comes from the
 * words the user typed (user.h); P_cache from the words of the current
 * conversation. Completions (words longer than the typed keys) get a fixed
 * cost penalty against exact matches.
 */

#pragma once

#include "textinput/model.h"
#include "textinput/user.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TI_CACHE_SIZE 48
#define TI_CAND_MAX   16
/* Context id for "unknown" (after a number, an unknown word, ...). */
#define TI_NONE       (-1)

struct ti_cand {
	int32_t id;   /* lexicon id or user id */
	int32_t cost; /* ranking cost, lower is better */
	bool exact;   /* key sequence has exactly the typed length */
};

struct ti_engine {
	const struct ti_model *m;
	struct ti_user *user; /* optional */
	int32_t cache[TI_CACHE_SIZE];
	uint8_t cache_n, cache_pos;
	/* Cost penalty of a completion against an exact match. */
	int32_t completion_penalty;
	/* Scratch: successor lists of the current context. */
	struct ti_list l1, l2;
};

void ti_engine_init(struct ti_engine *e, const struct ti_model *m, struct ti_user *user);

/* Remember a word of the conversation (typed or received). */
void ti_engine_cache_add(struct ti_engine *e, int32_t id);
void ti_engine_cache_clear(struct ti_engine *e);

/*
 * Candidates for the typed keys ('1'..'9'; empty: next-word prediction)
 * after the words p2 p1 (ids, model V = sentence start, TI_NONE = unknown).
 * Returns the number of candidates written to out, best first.
 */
int ti_predict(struct ti_engine *e, int32_t p2, int32_t p1, const char *keys, struct ti_cand *out,
	       int max);

/* Cost of one word in context (for evaluation and tests). */
int32_t ti_score(struct ti_engine *e, int32_t p2, int32_t p1, int32_t id);

/* Text of a candidate id (lexicon or user word). Returns the byte length. */
int ti_id_text(const struct ti_engine *e, int32_t id, char *buf, size_t size);

#ifdef __cplusplus
}
#endif
