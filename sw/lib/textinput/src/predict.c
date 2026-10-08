#include <math.h>
#include <string.h>

#include "textinput/predict.h"

#define W_LM         0.82f
#define W_USER_PAIR  0.08f
#define W_USER_WORD  0.05f
#define W_CACHE      0.05f
#define COST_INF     (64 * TI_COST_SCALE)
/* Completions: how many unigram-best words of the key range are considered. */
#define RANGE_TOP    24

void ti_engine_init(struct ti_engine *e, const struct ti_model *m, struct ti_user *user)
{
	memset(e, 0, sizeof(*e));
	e->m = m;
	e->user = user;
	e->completion_penalty = 5 * TI_COST_SCALE;
}

void ti_engine_cache_add(struct ti_engine *e, int32_t id)
{
	if (id < 0) {
		return;
	}
	e->cache[e->cache_pos] = id;
	e->cache_pos = (e->cache_pos + 1) % TI_CACHE_SIZE;
	if (e->cache_n < TI_CACHE_SIZE) {
		e->cache_n++;
	}
}

void ti_engine_cache_clear(struct ti_engine *e)
{
	e->cache_n = 0;
	e->cache_pos = 0;
}

static bool is_lex(const struct ti_engine *e, int32_t id)
{
	return id >= 0 && (uint32_t)id <= e->m->V; /* V = sentence start */
}

/* Load the successor lists of the context into l1 (bigram) and l2 (trigram). */
static void load_context(struct ti_engine *e, int32_t p2, int32_t p1)
{
	e->l1.found = false;
	e->l1.n = 0;
	e->l2.found = false;
	e->l2.n = 0;
	if (!is_lex(e, p1)) {
		return;
	}
	ti_bigram_list(e->m, p1, &e->l1);
	if (!e->l1.found || !is_lex(e, p2)) {
		return;
	}
	/* The trigram context is the index of the bigram (p2, p1). */
	ti_bigram_list(e->m, p2, &e->l2);
	int pos = e->l2.found ? ti_list_find(&e->l2, p1) : -1;

	if (pos < 0) {
		e->l2.found = false;
		e->l2.n = 0;
		return;
	}
	ti_trigram_list(e->m, e->l2.base + pos, &e->l2);
}

static int32_t lm_cost(const struct ti_engine *e, int32_t id)
{
	int32_t backoff = 0;
	int pos;

	if (id < 0 || (uint32_t)id >= e->m->V) {
		return COST_INF;
	}
	if (e->l2.found) {
		pos = ti_list_find(&e->l2, id);
		if (pos >= 0) {
			return e->l2.costs[pos];
		}
		backoff += e->l2.alpha;
	}
	if (e->l1.found) {
		pos = ti_list_find(&e->l1, id);
		if (pos >= 0) {
			return backoff + e->l1.costs[pos];
		}
		backoff += e->l1.alpha;
	}
	return backoff + ti_unigram_cost(e->m, id);
}

static int32_t cost_of(struct ti_engine *e, int32_t p1, int32_t id)
{
	float p = W_LM * exp2f(-(float)lm_cost(e, id) / TI_COST_SCALE);

	if (e->user) {
		uint32_t ctx_total;
		uint16_t pair = ti_user_pair(e->user, p1, id, &ctx_total);

		if (ctx_total) {
			p += W_USER_PAIR * pair / (ctx_total + 1.0f);
		}
		p += W_USER_WORD * ti_user_count(e->user, id) / (e->user->total + 20.0f);
	}
	if (e->cache_n) {
		int n = 0;

		for (int i = 0; i < e->cache_n; i++) {
			n += e->cache[i] == id;
		}
		p += W_CACHE * n / (e->cache_n + 10.0f);
	}
	if (p < 1e-15f) {
		return COST_INF;
	}
	return (int32_t)(-log2f(p) * TI_COST_SCALE);
}

int32_t ti_score(struct ti_engine *e, int32_t p2, int32_t p1, int32_t id)
{
	load_context(e, p2, p1);
	return cost_of(e, p1, id);
}

/* Best-first candidate list with a fixed capacity. */
struct topk {
	struct ti_cand *c;
	int n, max;
};

static void topk_add(struct topk *t, int32_t id, int32_t cost, bool exact)
{
	for (int i = 0; i < t->n; i++) {
		if (t->c[i].id == id) {
			return;
		}
	}
	if (t->n == t->max && cost >= t->c[t->n - 1].cost) {
		return;
	}
	int i = t->n < t->max ? t->n++ : t->n - 1;

	while (i > 0 && t->c[i - 1].cost > cost) {
		t->c[i] = t->c[i - 1];
		i--;
	}
	t->c[i] = (struct ti_cand){.id = id, .cost = cost, .exact = exact};
}

static void consider(struct ti_engine *e, struct topk *t, int32_t p1, int32_t id, bool exact)
{
	int32_t cost = cost_of(e, p1, id);

	if (cost >= COST_INF) {
		return;
	}
	topk_add(t, id, exact ? cost : cost + e->completion_penalty, exact);
}

/* Ids of a successor list inside [lo, hi). */
static void consider_list_range(struct ti_engine *e, struct topk *t, int32_t p1,
				const struct ti_list *l, uint32_t lo, uint32_t hi, uint32_t ex)
{
	if (!l->found) {
		return;
	}
	int i = 0, j = l->n;

	while (i < j) { /* first entry >= lo */
		int mid = (i + j) / 2;

		if (l->ids[mid] < lo) {
			i = mid + 1;
		} else {
			j = mid;
		}
	}
	for (; i < l->n && l->ids[i] < hi; i++) {
		consider(e, t, p1, l->ids[i], l->ids[i] < ex);
	}
}

int ti_predict(struct ti_engine *e, int32_t p2, int32_t p1, const char *keys, struct ti_cand *out,
	       int max)
{
	struct topk t = {.c = out, .n = 0, .max = max > TI_CAND_MAX ? TI_CAND_MAX : max};
	int32_t uids[16];
	bool uexact[16];

	load_context(e, p2, p1);

	if (!keys[0]) {
		/* Next word: successors of the context, frequent words, user habits. */
		for (int i = 0; i < e->l2.n; i++) {
			consider(e, &t, p1, e->l2.ids[i], true);
		}
		for (int i = 0; i < e->l1.n; i++) {
			consider(e, &t, p1, e->l1.ids[i], true);
		}
		for (int i = 0; i < e->m->ntop; i++) {
			consider(e, &t, p1, ti_top_word(e->m, i), true);
		}
		if (e->user) {
			int n = ti_user_successors(e->user, p1, uids, 16);

			for (int i = 0; i < n; i++) {
				consider(e, &t, p1, uids[i], true);
			}
		}
		for (int i = 0; i < e->cache_n; i++) {
			consider(e, &t, p1, e->cache[i], true);
		}
		return t.n;
	}

	uint32_t lo, ex, hi;

	ti_key_range(e->m, keys, &lo, &ex, &hi);

	/* All exact matches. */
	for (uint32_t id = lo; id < ex; id++) {
		consider(e, &t, p1, id, true);
	}
	/* Completions: in-context successors and the most frequent words. */
	consider_list_range(e, &t, p1, &e->l2, ex, hi, ex);
	consider_list_range(e, &t, p1, &e->l1, ex, hi, ex);

	int32_t best_id[RANGE_TOP];
	int32_t best_cost[RANGE_TOP];
	int nbest = 0;

	for (uint32_t id = ex; id < hi; id++) {
		int32_t c = ti_unigram_cost(e->m, id);

		if (nbest == RANGE_TOP && c >= best_cost[nbest - 1]) {
			continue;
		}
		int i = nbest < RANGE_TOP ? nbest++ : nbest - 1;

		while (i > 0 && best_cost[i - 1] > c) {
			best_cost[i] = best_cost[i - 1];
			best_id[i] = best_id[i - 1];
			i--;
		}
		best_cost[i] = c;
		best_id[i] = id;
	}
	for (int i = 0; i < nbest; i++) {
		consider(e, &t, p1, best_id[i], false);
	}

	if (e->user) {
		int n = ti_user_prefix(e->user, keys, uids, uexact, 16);

		for (int i = 0; i < n; i++) {
			consider(e, &t, p1, uids[i], uexact[i]);
		}
	}
	return t.n;
}

int ti_id_text(const struct ti_engine *e, int32_t id, char *buf, size_t size)
{
	if (id >= TI_USER_BASE) {
		const char *s = e->user ? ti_user_text(e->user, id) : NULL;

		if (!s) {
			buf[0] = '\0';
			return 0;
		}
		strncpy(buf, s, size - 1);
		buf[size - 1] = '\0';
		return strlen(buf);
	}
	return ti_word_utf8(e->m, id, buf, size);
}
