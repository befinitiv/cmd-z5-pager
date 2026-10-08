/*
 * Thin host wrapper around lib/textinput for the Python tools (ctypes).
 * Built by evaluate.py: gcc -O2 -shared -fPIC.
 */

#include <stdio.h>
#include <stdlib.h>

#include "textinput/predict.h"

struct host {
	struct ti_model m;
	struct ti_user user;
	struct ti_engine e;
	void *data;
};

void *tih_open(const char *path, int with_user)
{
	FILE *f = fopen(path, "rb");
	struct host *h;
	long size;

	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	h = calloc(1, sizeof(*h));
	h->data = malloc(size);
	if (fread(h->data, 1, size, f) != (size_t)size || ti_model_open(&h->m, h->data, size)) {
		fclose(f);
		free(h->data);
		free(h);
		return NULL;
	}
	fclose(f);
	ti_user_init(&h->user);
	ti_engine_init(&h->e, &h->m, with_user ? &h->user : NULL);
	return h;
}

int tih_vocab(void *hp)
{
	return ((struct host *)hp)->m.V;
}

void tih_set_penalty(void *hp, int bits64)
{
	((struct host *)hp)->e.completion_penalty = bits64;
}

int tih_find(void *hp, const char *word)
{
	struct host *h = hp;

	return h->e.user ? ti_user_find(h->e.user, &h->m, word) : ti_word_find(&h->m, word);
}

int tih_text(void *hp, int id, char *buf, int size)
{
	return ti_id_text(&((struct host *)hp)->e, id, buf, size);
}

int tih_keys(void *hp, int id, char *buf)
{
	return ti_word_keys(&((struct host *)hp)->m, id, buf);
}

int tih_predict(void *hp, int p2, int p1, const char *keys, int *ids, int *exact, int *costs,
		int max)
{
	struct host *h = hp;
	struct ti_cand c[TI_CAND_MAX];
	int n = ti_predict(&h->e, p2, p1, keys, c, max);

	for (int i = 0; i < n; i++) {
		ids[i] = c[i].id;
		exact[i] = c[i].exact;
		costs[i] = c[i].cost;
	}
	return n;
}

int tih_unigram(void *hp, int id)
{
	return ti_unigram_cost(&((struct host *)hp)->m, id);
}

void tih_range(void *hp, const char *keys, unsigned *lo, unsigned *ex, unsigned *hi)
{
	ti_key_range(&((struct host *)hp)->m, keys, lo, ex, hi);
}

int tih_learn(void *hp, int prev, const char *word)
{
	struct host *h = hp;
	int id = ti_user_learn(&h->user, &h->m, prev, word);

	ti_engine_cache_add(&h->e, id);
	return id;
}
