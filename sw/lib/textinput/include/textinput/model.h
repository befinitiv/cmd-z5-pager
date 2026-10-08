/*
 * Read-only access to a predictive text model (.lm, built by tools/lmbuild).
 *
 * The model is used in place (e.g. memory-mapped flash); ti_model_open() only
 * parses the header. Plain C, no dynamic memory, no Zephyr dependency.
 *
 * Word ids are 0 .. V-1 in key sequence order, so all words whose key
 * sequence starts with a given prefix form one id range. Id V stands for the
 * sentence start (only valid as context). Costs are -log2(p) in units of
 * 1/TI_COST_SCALE bit.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TI_COST_SCALE 64
#define TI_MAX_SYMS   31
/* Enough for TI_MAX_SYMS two-byte UTF-8 characters plus NUL. */
#define TI_WORD_BYTES 64
#define TI_MAX_KEYS   TI_MAX_SYMS
/* Successors decoded per context; longer lists are cut. */
#define TI_LIST_MAX   512

enum ti_case {
	TI_CASE_LOWER,
	TI_CASE_CAPITAL,
	TI_CASE_UPPER,
};

struct ti_huff {
	uint16_t first[16];
	uint8_t count[16];
	uint8_t index[16];
	uint8_t syms[64];
};

struct ti_postings {
	uint32_t nkeys;
	uint32_t nentries;
	uint8_t kk, ki, anchor_every, qbits;
	int16_t cost_cb[16];
	int16_t alpha_cb[16];
	uint32_t nanchors;
	const uint8_t *anchors; /* nanchors x (u32 key, u32 bitpos, u32 entry base) */
	const uint8_t *bits;
};

struct ti_model {
	const uint8_t *data;
	size_t size;
	char lang[3];
	uint32_t V;
	uint8_t nalpha;
	uint8_t block;
	uint16_t alphabet[64]; /* codepoints of the lowercase symbols */
	char keys[64];         /* phone key ('1'..'9') of each symbol */

	uint32_t nblocks;
	const uint8_t *lex_offsets;
	const uint8_t *lex_bits;
	struct ti_huff h_pre, h_sym, h_case;

	const uint8_t *uni;
	struct ti_postings bi, tri;
	const uint8_t *top;
	uint16_t ntop;
};

/* Successor list of one context. */
struct ti_list {
	bool found;
	uint32_t base;  /* global index of the first entry (bigram index for trigrams) */
	int16_t alpha;  /* backoff cost when a word is not in the list */
	uint16_t n;
	uint16_t ids[TI_LIST_MAX];
	int16_t costs[TI_LIST_MAX];
};

/* Returns 0 or a negative error if the data is not a valid model. */
int ti_model_open(struct ti_model *m, const void *data, size_t size);

/* Lowercase symbols of a word (alphabet indices); returns the length. */
int ti_word_syms(const struct ti_model *m, uint32_t id, uint8_t *syms, enum ti_case *c);
/* Word as UTF-8 in its stored case. Returns the byte length. */
int ti_word_utf8(const struct ti_model *m, uint32_t id, char *buf, size_t size);
/* Key sequence of a word ('1'..'9'), NUL-terminated. Returns its length. */
int ti_word_keys(const struct ti_model *m, uint32_t id, char *keys);

/*
 * Ids whose key sequence starts with `keys`: [*lo, *hi). The exact matches
 * (same length) are [*lo, *exact_hi).
 */
void ti_key_range(const struct ti_model *m, const char *keys, uint32_t *lo, uint32_t *exact_hi,
		  uint32_t *hi);

/*
 * Id of a word given in UTF-8. Prefers the same case, else any case
 * variant. Returns -1 if the word is not in the lexicon.
 */
int32_t ti_word_find(const struct ti_model *m, const char *utf8);

int32_t ti_unigram_cost(const struct ti_model *m, uint32_t id);

/* Successors of a word (or of the sentence start, id V). */
void ti_bigram_list(const struct ti_model *m, uint32_t ctx, struct ti_list *out);
/* Successors of a word pair, given by the global index of the bigram entry. */
void ti_trigram_list(const struct ti_model *m, uint32_t bigram_index, struct ti_list *out);
/* Position of id in a sorted list, or -1. */
int ti_list_find(const struct ti_list *l, uint32_t id);

uint16_t ti_top_word(const struct ti_model *m, int i);

/* Shared helpers */
uint32_t ti_utf8_next(const char **s);
int ti_utf8_put(char *buf, uint32_t cp);
uint32_t ti_to_lower(uint32_t cp);
uint32_t ti_to_upper(uint32_t cp);
/* Phone key of a character ('1'..'9', '0' for space), or 0 if none. */
char ti_key_of_char(uint32_t cp);

#ifdef __cplusplus
}
#endif
