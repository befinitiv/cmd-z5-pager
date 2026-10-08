#include <errno.h>
#include <string.h>

#include "bits.h"
#include "textinput/model.h"

#define SEC_LEX 1
#define SEC_UNI 2
#define SEC_BI  3
#define SEC_TRI 4
#define SEC_TOP 5

#define UNI_BITS 6
/* Unigram costs are stored in steps of half a bit. */
#define UNI_UNIT (TI_COST_SCALE / 2)

/* Characters -------------------------------------------------------------- */

uint32_t ti_utf8_next(const char **s)
{
	const uint8_t *p = (const uint8_t *)*s;
	uint32_t cp;
	int extra;

	if (p[0] < 0x80) {
		cp = p[0];
		extra = 0;
	} else if ((p[0] & 0xe0) == 0xc0) {
		cp = p[0] & 0x1f;
		extra = 1;
	} else if ((p[0] & 0xf0) == 0xe0) {
		cp = p[0] & 0x0f;
		extra = 2;
	} else if ((p[0] & 0xf8) == 0xf0) {
		cp = p[0] & 0x07;
		extra = 3;
	} else {
		*s += 1;
		return 0xfffd;
	}
	for (int i = 1; i <= extra; i++) {
		if ((p[i] & 0xc0) != 0x80) {
			*s += i;
			return 0xfffd;
		}
		cp = (cp << 6) | (p[i] & 0x3f);
	}
	*s += extra + 1;
	return cp;
}

int ti_utf8_put(char *buf, uint32_t cp)
{
	if (cp < 0x80) {
		buf[0] = cp;
		return 1;
	}
	if (cp < 0x800) {
		buf[0] = 0xc0 | (cp >> 6);
		buf[1] = 0x80 | (cp & 0x3f);
		return 2;
	}
	buf[0] = 0xe0 | (cp >> 12);
	buf[1] = 0x80 | ((cp >> 6) & 0x3f);
	buf[2] = 0x80 | (cp & 0x3f);
	return 3;
}

uint32_t ti_to_lower(uint32_t cp)
{
	if ((cp >= 'A' && cp <= 'Z') || (cp >= 0xc0 && cp <= 0xde && cp != 0xd7)) {
		return cp + 32;
	}
	if (cp == 0x152) {
		return 0x153; /* Œ */
	}
	if (cp == 0x178) {
		return 0xff; /* Ÿ */
	}
	return cp;
}

uint32_t ti_to_upper(uint32_t cp)
{
	if ((cp >= 'a' && cp <= 'z') || (cp >= 0xe0 && cp <= 0xfe && cp != 0xf7)) {
		return cp - 32;
	}
	if (cp == 0x153) {
		return 0x152;
	}
	if (cp == 0xff) {
		return 0x178;
	}
	return cp; /* includes ß, which has no single uppercase letter */
}

char ti_key_of_char(uint32_t cp)
{
	/* Base letters of U+00E0..U+00FF, '?' = no letter. */
	static const char latin1[] = "aaaaaaaceeeeiiiidnooooo?ouuuuyty";
	static const char itu[] = "22233344455566677778889999";

	cp = ti_to_lower(cp);
	if (cp == ' ') {
		return '0';
	}
	if (cp == '\'' || cp == '-') {
		return '1';
	}
	if (cp == 0xdf) {
		cp = 's';
	} else if (cp == 0x153) {
		cp = 'o';
	} else if (cp >= 0xe0 && cp <= 0xff) {
		cp = latin1[cp - 0xe0];
	}
	if (cp >= 'a' && cp <= 'z') {
		return itu[cp - 'a'];
	}
	return 0;
}

/* Header ------------------------------------------------------------------ */

static void huff_init(struct ti_huff *h, const uint8_t *lengths, int nsym)
{
	uint16_t code = 0;
	int idx = 0;

	memset(h, 0, sizeof(*h));
	for (int len = 1; len <= 15; len++) {
		h->first[len] = code;
		h->index[len] = idx;
		for (int s = 0; s < nsym; s++) {
			if (lengths[s] == len) {
				h->syms[idx++] = s;
				h->count[len]++;
				code++;
			}
		}
		code <<= 1;
	}
}

static int huff_decode(const struct ti_huff *h, struct ti_bits *b)
{
	uint16_t code = 0;

	for (int len = 1; len <= 15; len++) {
		code = (code << 1) | ti_bit(b);
		if (code - h->first[len] < h->count[len]) {
			return h->syms[h->index[len] + code - h->first[len]];
		}
	}
	return -1;
}

static void postings_init(struct ti_postings *p, const uint8_t *d)
{
	p->nkeys = ti_get_u32(d);
	p->nentries = ti_get_u32(d + 4);
	p->kk = d[8];
	p->ki = d[9];
	p->anchor_every = d[10];
	p->qbits = d[11];
	d += 12;
	for (int i = 0; i < 16; i++) {
		p->cost_cb[i] = ti_get_i16(d + 2 * i);
		p->alpha_cb[i] = ti_get_i16(d + 32 + 2 * i);
	}
	d += 64;
	p->nanchors = ti_get_u32(d);
	p->anchors = d + 4;
	d += 4 + 12 * p->nanchors;
	p->bits = d + 4; /* after the u32 bit count */
}

int ti_model_open(struct ti_model *m, const void *data, size_t size)
{
	const uint8_t *d = data;

	memset(m, 0, sizeof(*m));
	if (size < 16 + 128 + 64 || memcmp(d, "CZLM", 4) != 0 || ti_get_u16(d + 4) != 1) {
		return -EINVAL;
	}
	m->data = d;
	m->size = size;
	uint16_t nsec = ti_get_u16(d + 6);

	memcpy(m->lang, d + 8, 2);
	m->V = ti_get_u32(d + 12);
	m->nalpha = d[16];
	m->block = d[17];
	for (int i = 0; i < 64; i++) {
		m->alphabet[i] = ti_get_u16(d + 20 + 2 * i);
		m->keys[i] = d[148 + i];
	}

	const uint8_t *dir = d + 212;

	for (int i = 0; i < nsec; i++) {
		uint32_t id = ti_get_u32(dir + 12 * i);
		uint32_t off = ti_get_u32(dir + 12 * i + 4);
		uint32_t len = ti_get_u32(dir + 12 * i + 8);
		const uint8_t *s = d + off;

		if (off + len > size) {
			return -EINVAL;
		}
		switch (id) {
		case SEC_LEX:
			m->nblocks = ti_get_u32(s);
			huff_init(&m->h_pre, s + 4, 32);
			huff_init(&m->h_sym, s + 36, 64);
			huff_init(&m->h_case, s + 100, 4);
			m->lex_offsets = s + 104;
			m->lex_bits = s + 104 + 4 * m->nblocks;
			break;
		case SEC_UNI:
			m->uni = s;
			break;
		case SEC_BI:
			postings_init(&m->bi, s);
			break;
		case SEC_TRI:
			postings_init(&m->tri, s);
			break;
		case SEC_TOP:
			m->ntop = ti_get_u16(s);
			m->top = s + 2;
			break;
		default:
			break;
		}
	}
	if (!m->lex_bits || !m->uni) {
		return -EINVAL;
	}
	return 0;
}

/* Lexicon ----------------------------------------------------------------- */

/* Decoding position inside a lexicon block. */
struct lex_cursor {
	struct ti_bits b;
	uint32_t id;
	uint8_t syms[TI_MAX_SYMS];
	uint8_t len;
	uint8_t cas;
};

static void lex_block(const struct ti_model *m, uint32_t block, struct lex_cursor *c)
{
	c->b.data = m->lex_bits;
	c->b.pos = ti_get_u32(m->lex_offsets + 4 * block);
	c->id = block * m->block;
	c->len = 0;
}

/* Decode the word at c->id and advance. */
static void lex_next(const struct ti_model *m, struct lex_cursor *c)
{
	int eow = m->nalpha;
	int pre = 0;

	if (c->id % m->block) {
		pre = huff_decode(&m->h_pre, &c->b);
	}
	c->len = pre;
	for (;;) {
		int s = huff_decode(&m->h_sym, &c->b);

		if (s < 0 || s == eow || c->len >= TI_MAX_SYMS) {
			break;
		}
		c->syms[c->len++] = s;
	}
	c->cas = huff_decode(&m->h_case, &c->b);
	c->id++;
}

static void lex_seek(const struct ti_model *m, uint32_t id, struct lex_cursor *c)
{
	lex_block(m, id / m->block, c);
	while (c->id <= id) {
		lex_next(m, c);
	}
}

int ti_word_syms(const struct ti_model *m, uint32_t id, uint8_t *syms, enum ti_case *cas)
{
	struct lex_cursor c;

	if (id >= m->V) {
		return 0;
	}
	lex_seek(m, id, &c);
	memcpy(syms, c.syms, c.len);
	if (cas) {
		*cas = c.cas;
	}
	return c.len;
}

static int syms_to_utf8(const struct ti_model *m, const uint8_t *syms, int n, enum ti_case cas,
			char *buf, size_t size)
{
	int out = 0;

	for (int i = 0; i < n; i++) {
		uint32_t cp = m->alphabet[syms[i]];

		if (cas == TI_CASE_UPPER || (cas == TI_CASE_CAPITAL && i == 0)) {
			cp = ti_to_upper(cp);
		}
		if (out + 4 >= (int)size) {
			break;
		}
		out += ti_utf8_put(buf + out, cp);
	}
	buf[out] = '\0';
	return out;
}

int ti_word_utf8(const struct ti_model *m, uint32_t id, char *buf, size_t size)
{
	uint8_t syms[TI_MAX_SYMS];
	enum ti_case cas;
	int n = ti_word_syms(m, id, syms, &cas);

	return syms_to_utf8(m, syms, n, cas, buf, size);
}

int ti_word_keys(const struct ti_model *m, uint32_t id, char *keys)
{
	uint8_t syms[TI_MAX_SYMS];
	int n = ti_word_syms(m, id, syms, NULL);

	for (int i = 0; i < n; i++) {
		keys[i] = m->keys[syms[i]];
	}
	keys[n] = '\0';
	return n;
}

/* Compare a word's key sequence with a target string (memcmp-like). */
static int cmp_keys(const struct ti_model *m, const struct lex_cursor *c, const char *t)
{
	int i = 0;

	for (; i < c->len && t[i]; i++) {
		char k = m->keys[c->syms[i]];

		if (k != t[i]) {
			return k < t[i] ? -1 : 1;
		}
	}
	if (i == c->len && !t[i]) {
		return 0;
	}
	return i == c->len ? -1 : 1;
}

/* First id whose key sequence is >= t. */
static uint32_t lower_bound(const struct ti_model *m, const char *t)
{
	struct lex_cursor c;
	uint32_t lo = 0, hi = m->nblocks;

	/* Last block whose first word is < t. */
	while (hi - lo > 1) {
		uint32_t mid = (lo + hi) / 2;

		lex_block(m, mid, &c);
		lex_next(m, &c);
		if (cmp_keys(m, &c, t) < 0) {
			lo = mid;
		} else {
			hi = mid;
		}
	}
	lex_block(m, lo, &c);
	while (c.id < m->V) {
		uint32_t id = c.id;

		lex_next(m, &c);
		if (cmp_keys(m, &c, t) >= 0) {
			return id;
		}
	}
	return m->V;
}

void ti_key_range(const struct ti_model *m, const char *keys, uint32_t *lo, uint32_t *exact_hi,
		  uint32_t *hi)
{
	char t[TI_MAX_KEYS + 2];
	size_t n = strlen(keys);

	if (n > TI_MAX_KEYS) {
		*lo = *exact_hi = *hi = 0;
		return;
	}
	memcpy(t, keys, n);
	*lo = lower_bound(m, keys);
	/* Keys are '1'..'9': "0" sorts after the exact match, ":" after all extensions. */
	t[n] = '0';
	t[n + 1] = '\0';
	*exact_hi = lower_bound(m, t);
	t[n] = ':';
	*hi = lower_bound(m, t);
}

int32_t ti_word_find(const struct ti_model *m, const char *utf8)
{
	uint8_t syms[TI_MAX_SYMS];
	char keys[TI_MAX_KEYS + 1];
	int n = 0;
	enum ti_case want = TI_CASE_LOWER;
	bool all_upper = true;
	const char *p = utf8;

	while (*p) {
		uint32_t cp = ti_utf8_next(&p);
		uint32_t lc = ti_to_lower(cp);
		int s;

		for (s = 0; s < m->nalpha && m->alphabet[s] != lc; s++) {
		}
		if (s == m->nalpha || n == TI_MAX_SYMS) {
			return -1;
		}
		if (lc != cp && n == 0) {
			want = TI_CASE_CAPITAL;
		}
		if (lc == cp) {
			all_upper = false;
		}
		syms[n] = s;
		keys[n++] = m->keys[s];
	}
	if (n == 0) {
		return -1;
	}
	if (all_upper && n > 1) {
		want = TI_CASE_UPPER;
	}
	keys[n] = '\0';

	uint32_t lo, ex, hi;
	struct lex_cursor c;
	int32_t any = -1;

	ti_key_range(m, keys, &lo, &ex, &hi);
	if (lo >= ex) {
		return -1;
	}
	lex_seek(m, lo, &c);
	for (uint32_t id = lo;;) {
		if (c.len == n && memcmp(c.syms, syms, n) == 0) {
			if (c.cas == want) {
				return id;
			}
			if (any < 0) {
				any = id;
			}
		}
		if (++id >= ex) {
			break;
		}
		lex_next(m, &c);
	}
	return any;
}

int32_t ti_unigram_cost(const struct ti_model *m, uint32_t id)
{
	uint32_t bit = id * UNI_BITS;
	struct ti_bits b = {.data = m->uni, .pos = bit};

	return (int32_t)ti_read(&b, UNI_BITS) * UNI_UNIT;
}

uint16_t ti_top_word(const struct ti_model *m, int i)
{
	return ti_get_u16(m->top + 2 * i);
}

/* N-gram tables ------------------------------------------------------------ */

static void postings_lookup(const struct ti_postings *p, uint32_t key, struct ti_list *out)
{
	out->found = false;
	out->n = 0;
	out->alpha = 0;
	if (p->nanchors == 0) {
		return;
	}

	/* Last anchor with key <= target. */
	uint32_t lo = 0, hi = p->nanchors;

	while (hi - lo > 1) {
		uint32_t mid = (lo + hi) / 2;

		if (ti_get_u32(p->anchors + 12 * mid) <= key) {
			lo = mid;
		} else {
			hi = mid;
		}
	}
	const uint8_t *a = p->anchors + 12 * lo;
	uint32_t k = ti_get_u32(a);
	struct ti_bits b = {.data = p->bits, .pos = ti_get_u32(a + 4)};
	uint32_t base = ti_get_u32(a + 8);

	if (k > key) {
		return;
	}
	for (uint32_t r = lo * p->anchor_every; r < p->nkeys; r++) {
		if (r != lo * p->anchor_every) {
			if (r % p->anchor_every == 0) {
				return; /* reached the next anchor */
			}
			k += ti_rice(&b, p->kk) + 1;
		}
		if (k > key) {
			return;
		}
		uint32_t aq = ti_read(&b, p->qbits);
		uint32_t n = ti_gamma(&b);
		int32_t id = -1;

		if (k == key) {
			out->found = true;
			out->base = base;
			out->alpha = p->alpha_cb[aq];
			for (uint32_t i = 0; i < n; i++) {
				id += ti_rice(&b, p->ki) + 1;
				uint32_t cq = ti_read(&b, p->qbits);

				if (out->n < TI_LIST_MAX) {
					out->ids[out->n] = id;
					out->costs[out->n] = p->cost_cb[cq];
					out->n++;
				}
			}
			return;
		}
		for (uint32_t i = 0; i < n; i++) {
			ti_rice(&b, p->ki);
			ti_read(&b, p->qbits);
		}
		base += n;
	}
}

void ti_bigram_list(const struct ti_model *m, uint32_t ctx, struct ti_list *out)
{
	postings_lookup(&m->bi, ctx, out);
}

void ti_trigram_list(const struct ti_model *m, uint32_t bigram_index, struct ti_list *out)
{
	postings_lookup(&m->tri, bigram_index, out);
}

int ti_list_find(const struct ti_list *l, uint32_t id)
{
	int lo = 0, hi = l->n;

	while (lo < hi) {
		int mid = (lo + hi) / 2;

		if (l->ids[mid] < id) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return lo < l->n && l->ids[lo] == id ? lo : -1;
}
