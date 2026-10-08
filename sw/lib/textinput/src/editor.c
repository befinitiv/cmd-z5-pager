#include <string.h>

#include "textinput/editor.h"

static const char *const multitap_de[10] = {
	" 0", ".,?!'-:;()@/1", "abcä2", "def3", "ghi4", "jkl5", "mnoö6", "pqrsß7", "tuvü8",
	"wxyz9",
};

static const char *const multitap_en[10] = {
	" 0", ".,?!'-:;()@/1", "abc2", "def3", "ghi4", "jkl5", "mno6", "pqrs7", "tuv8", "wxyz9",
};

static const char *const symbols[] = {
	".", ",", "?", "!", "'", "\"", "-", ":", ";", "(", ")", "@", "/", "&", "%", "+", "=",
	"*", "#", "€", "$", "_", "~", "é", "è", "à", "ç", "ñ", ":-)", ";-)", ":-(", ":-D",
};

#define NSYMBOLS ((int)(sizeof(symbols) / sizeof(symbols[0])))

const char *const *ti_multitap_table(const char *lang)
{
	return strcmp(lang, "de") == 0 ? multitap_de : multitap_en;
}

int ti_editor_symbol_count(void)
{
	return NSYMBOLS;
}

const char *ti_editor_symbol(int i)
{
	return i >= 0 && i < NSYMBOLS ? symbols[i] : "";
}

/* Characters -------------------------------------------------------------- */

static bool is_letter(uint32_t cp)
{
	char k = ti_key_of_char(cp);

	return k >= '2' && k <= '9';
}

static bool is_word_char(uint32_t cp)
{
	return is_letter(cp) || cp == '\'' || cp == '-';
}

/* Punctuation the model treats as transparent (does not break the context). */
static bool is_transparent(uint32_t cp)
{
	return cp == ' ' || cp == ',' || cp == ';' || cp == ':' || cp == '"' || cp == '(' ||
	       cp == ')';
}

static bool is_sentence_end(uint32_t cp)
{
	return cp == '.' || cp == '!' || cp == '?';
}

/* Start of the UTF-8 character that ends at pos. */
static int char_start(const char *text, int pos)
{
	do {
		pos--;
	} while (pos > 0 && ((uint8_t)text[pos] & 0xc0) == 0x80);
	return pos;
}

static uint32_t char_before(const char *text, int pos)
{
	const char *p = text + char_start(text, pos);

	return ti_utf8_next(&p);
}

/* Text helpers ------------------------------------------------------------- */

static bool at_sentence_start(const struct ti_editor *ed)
{
	int pos = ed->len;

	while (pos > 0 && ed->text[pos - 1] == ' ') {
		pos--;
	}
	return pos == 0 || is_sentence_end(char_before(ed->text, pos));
}

static bool append(struct ti_editor *ed, const char *s)
{
	size_t n = strlen(s);

	if (ed->len + n > TI_TEXT_MAX) {
		return false;
	}
	memcpy(ed->text + ed->len, s, n + 1);
	ed->len += n;
	return true;
}

static void truncate_to(struct ti_editor *ed, int len)
{
	ed->len = len;
	ed->text[len] = '\0';
}

/* Apply the case mode to a word (UTF-8). */
static void apply_case(const struct ti_editor *ed, const char *src, char *dst, size_t size,
		       bool sentence_start)
{
	size_t out = 0;
	bool first = true;

	while (*src && out + 4 < size) {
		uint32_t cp = ti_utf8_next(&src);

		if (ed->shift == TI_SHIFT_UPPER ||
		    (first && ed->shift == TI_SHIFT_AUTO && sentence_start)) {
			cp = ti_to_upper(cp);
		}
		out += ti_utf8_put(dst + out, cp);
		first = false;
	}
	dst[out] = '\0';
}

/* Word ending at pos, copied to buf; returns its start or -1. */
static int word_before(const struct ti_editor *ed, int pos, char *buf, size_t size)
{
	int start = pos;

	while (start > 0 && is_word_char(char_before(ed->text, start))) {
		start = char_start(ed->text, start);
	}
	if (start == pos || (size_t)(pos - start) >= size) {
		return -1;
	}
	memcpy(buf, ed->text + start, pos - start);
	buf[pos - start] = '\0';
	return start;
}

/* Context (two previous words) of a word that starts at pos. */
static void context_at(const struct ti_editor *ed, int pos, int32_t *p2, int32_t *p1)
{
	int32_t bos = ed->eng->m->V;
	int32_t ctx[2] = {TI_NONE, TI_NONE};
	char word[TI_WORD_BYTES];

	for (int i = 0; i < 2; i++) {
		while (pos > 0 && is_transparent(char_before(ed->text, pos))) {
			pos = char_start(ed->text, pos);
		}
		if (pos == 0 || is_sentence_end(char_before(ed->text, pos))) {
			ctx[i] = bos;
			break;
		}
		int start = word_before(ed, pos, word, sizeof(word));

		if (start < 0) {
			break; /* number or symbol: unknown context */
		}
		ctx[i] = ed->eng->user ? ti_user_find(ed->eng->user, ed->eng->m, word)
				       : ti_word_find(ed->eng->m, word);
		if (ctx[i] < 0) {
			ctx[i] = TI_NONE;
			break;
		}
		pos = start;
	}
	*p1 = ctx[0];
	*p2 = ctx[0] == bos ? TI_NONE : ctx[1];
}

/* Learn the word that ends at the end of the text, if any. */
static void learn_last_word(struct ti_editor *ed)
{
	char word[TI_WORD_BYTES];
	int start = word_before(ed, ed->len, word, sizeof(word));
	int32_t p2, p1, id;

	if (start < 0) {
		return;
	}
	context_at(ed, start, &p2, &p1);
	if (ed->learn && ed->eng->user) {
		id = ti_user_learn(ed->eng->user, ed->eng->m, p1, word);
	} else {
		id = ti_word_find(ed->eng->m, word);
	}
	ti_engine_cache_add(ed->eng, id);
}

/* Append a separator (space, punctuation, digit) after learning the word before it. */
static bool append_sep(struct ti_editor *ed, const char *s)
{
	learn_last_word(ed);
	return append(ed, s);
}

/* Candidates --------------------------------------------------------------- */

static void refresh(struct ti_editor *ed)
{
	char raw[TI_WORD_BYTES];
	bool start = at_sentence_start(ed);

	context_at(ed, ed->len, &ed->p2, &ed->p1);
	ed->ncands = 0;
	ed->sel = -1;
	if (ed->mode != TI_MODE_T9 || ed->pend_len || ed->symbols) {
		return;
	}
	ed->keys[ed->nkeys] = '\0';
	ed->ncands = ti_predict(ed->eng, ed->p2, ed->p1, ed->keys, ed->cands, TI_EDIT_CANDS);
	for (int i = 0; i < ed->ncands; i++) {
		ti_id_text(ed->eng, ed->cands[i].id, raw, sizeof(raw));
		apply_case(ed, raw, ed->cand_text[i], sizeof(ed->cand_text[i]), start);
	}
	if (ed->nkeys && ed->ncands) {
		ed->sel = 0;
	}
}

/* Insert the selected candidate (with a space). */
static bool commit_candidate(struct ti_editor *ed, bool space)
{
	if (ed->sel < 0 || ed->sel >= ed->ncands) {
		return false;
	}
	/* A predicted word right after a word or punctuation gets a space first. */
	if (ed->len && !ed->nkeys && !is_transparent(char_before(ed->text, ed->len))) {
		append_sep(ed, " ");
	}
	if (!append(ed, ed->cand_text[ed->sel])) {
		return false;
	}
	ed->nkeys = 0;
	if (space) {
		append_sep(ed, " ");
	}
	return true;
}

static void finish_pending(struct ti_editor *ed)
{
	ed->pend_len = 0;
	ed->pend_key = 0;
}

/* End the current word in predictive mode without a space. */
static void finish_word(struct ti_editor *ed)
{
	if (ed->nkeys) {
		if (ed->ncands) {
			commit_candidate(ed, false);
		}
		ed->nkeys = 0;
	}
	finish_pending(ed);
}

/* Multitap ----------------------------------------------------------------- */

static uint32_t nth_char(const char *s, int n, int *count)
{
	uint32_t cp = 0;
	int i = 0;

	while (*s) {
		uint32_t c = ti_utf8_next(&s);

		if (i == n) {
			cp = c;
		}
		i++;
	}
	*count = i;
	return cp;
}

static void multitap(struct ti_editor *ed, char key, uint32_t now)
{
	const char *chars = ed->multitap[key - '0'];
	int count;
	char buf[5];

	if (ed->pend_len && ed->pend_key == key && now - ed->pend_time < TI_MULTITAP_MS) {
		truncate_to(ed, ed->len - ed->pend_len);
		ed->pend_idx++;
	} else {
		finish_pending(ed);
		ed->pend_idx = 0;
	}
	uint32_t cp = nth_char(chars, ed->pend_idx, &count);

	if (ed->pend_idx >= count) {
		ed->pend_idx = 0;
		cp = nth_char(chars, 0, &count);
	}
	if (is_letter(cp) && (ed->shift == TI_SHIFT_UPPER ||
			      (ed->shift == TI_SHIFT_AUTO && at_sentence_start(ed)))) {
		cp = ti_to_upper(cp);
	}
	buf[ti_utf8_put(buf, cp)] = '\0';

	bool fresh = ed->pend_len == 0;

	/* Punctuation goes right after the word: drop the space added after it. */
	if (fresh && key == '1' && ed->len && ed->text[ed->len - 1] == ' ') {
		truncate_to(ed, ed->len - 1);
	}
	bool ok = fresh && !is_word_char(cp) ? append_sep(ed, buf) : append(ed, buf);

	if (ok) {
		ed->pend_key = key;
		ed->pend_len = strlen(buf);
		ed->pend_time = now;
	}
}

/* Public API --------------------------------------------------------------- */

void ti_editor_init(struct ti_editor *ed, struct ti_engine *eng, const char *lang)
{
	memset(ed, 0, sizeof(*ed));
	ed->learn = true;
	ti_editor_set_engine(ed, eng, lang);
}

void ti_editor_set_engine(struct ti_editor *ed, struct ti_engine *eng, const char *lang)
{
	ed->eng = eng;
	ed->multitap = ti_multitap_table(lang);
	ed->nkeys = 0;
	finish_pending(ed);
	refresh(ed);
}

void ti_editor_clear(struct ti_editor *ed)
{
	truncate_to(ed, 0);
	ed->nkeys = 0;
	ed->symbols = false;
	finish_pending(ed);
	ti_engine_cache_clear(ed->eng);
	refresh(ed);
}

static void delete_char(struct ti_editor *ed)
{
	if (ed->len) {
		truncate_to(ed, char_start(ed->text, ed->len));
	}
}

static void delete_word(struct ti_editor *ed)
{
	while (ed->len && ed->text[ed->len - 1] == ' ') {
		delete_char(ed);
	}
	while (ed->len && ed->text[ed->len - 1] != ' ') {
		delete_char(ed);
	}
}

static bool key_clear(struct ti_editor *ed, bool long_press)
{
	if (ed->symbols) {
		ed->symbols = false;
	} else if (long_press) {
		if (!ed->len && !ed->nkeys) {
			return false;
		}
		ed->nkeys = 0;
		finish_pending(ed);
		delete_word(ed);
	} else if (ed->nkeys) {
		ed->nkeys--;
	} else if (ed->pend_len) {
		truncate_to(ed, ed->len - ed->pend_len);
		finish_pending(ed);
	} else if (ed->sel >= 0 && !ed->nkeys) {
		ed->sel = -1; /* only deselect the prediction */
		return true;
	} else if (ed->len) {
		delete_char(ed);
	} else {
		return false;
	}
	refresh(ed);
	return true;
}

static void insert_literal(struct ti_editor *ed, const char *s)
{
	finish_word(ed);
	append_sep(ed, s);
}

bool ti_editor_key(struct ti_editor *ed, enum ti_ekey key, bool long_press, uint32_t now_ms)
{
	char digit = '0' + key;

	if (key == TI_EK_CLEAR) {
		return key_clear(ed, long_press);
	}
	ed->symbols = false;

	if (key == TI_EK_POUND) {
		if (long_press) {
			finish_word(ed);
			if (ed->lang_cb) {
				ed->lang_cb(ed->lang_arg);
			}
		} else {
			finish_word(ed);
			ed->mode = (ed->mode + 1) % 3;
		}
		refresh(ed);
		return true;
	}

	if (key == TI_EK_STAR) {
		if (long_press) {
			ed->shift = (ed->shift + 1) % 3;
		} else if (ed->mode == TI_MODE_123) {
			insert_literal(ed, "*");
		} else if (ed->ncands && ed->sel >= 0) {
			ed->sel = (ed->sel + 1) % ed->ncands;
			return true;
		} else {
			return true;
		}
		refresh(ed);
		return true;
	}

	/* Digits. */
	if (long_press || ed->mode == TI_MODE_123) {
		char s[2] = {digit, '\0'};

		if (long_press && digit == '0' && ed->mode == TI_MODE_123) {
			s[0] = '+';
		}
		insert_literal(ed, s);
		refresh(ed);
		return true;
	}

	if (ed->mode == TI_MODE_ABC || digit == '1') {
		if (ed->mode == TI_MODE_T9 && ed->nkeys) {
			finish_word(ed);
		}
		multitap(ed, digit, now_ms);
		refresh(ed);
		return true;
	}

	/* Predictive mode. */
	if (digit == '0') {
		if (ed->nkeys) {
			if (ed->ncands) {
				commit_candidate(ed, true);
			}
			/* No word for these keys: keep them, the user corrects. */
		} else if (ed->sel >= 0) {
			commit_candidate(ed, true);
		} else {
			finish_pending(ed);
			append_sep(ed, " ");
		}
		refresh(ed);
		return true;
	}
	finish_pending(ed);
	if (ed->nkeys < TI_MAX_KEYS) {
		ed->keys[ed->nkeys++] = digit;
	}
	refresh(ed);
	return true;
}

bool ti_editor_wheel(struct ti_editor *ed, int delta)
{
	if (ed->symbols) {
		int n = NSYMBOLS;

		ed->sym_sel = ((ed->sym_sel - delta) % n + n) % n;
		return true;
	}
	if (!ed->ncands) {
		return false;
	}
	/* Up = towards the first candidate; from "none" down selects the first. */
	int sel = ed->sel - delta;

	if (sel < (ed->nkeys ? 0 : -1)) {
		sel = ed->ncands - 1;
	} else if (sel >= ed->ncands) {
		sel = ed->nkeys ? 0 : -1;
	}
	ed->sel = sel;
	return true;
}

bool ti_editor_select(struct ti_editor *ed)
{
	if (ed->symbols) {
		ed->symbols = false;
		insert_literal(ed, symbols[ed->sym_sel]);
		refresh(ed);
		return true;
	}
	if (ed->sel < 0) {
		return false;
	}
	commit_candidate(ed, true);
	refresh(ed);
	return true;
}

void ti_editor_symbols(struct ti_editor *ed, bool open)
{
	ed->symbols = open;
	if (open) {
		finish_word(ed);
		ed->sym_sel = 0;
	}
	refresh(ed);
}

bool ti_editor_tick(struct ti_editor *ed, uint32_t now_ms)
{
	if (ed->pend_len && now_ms - ed->pend_time >= TI_MULTITAP_MS) {
		finish_pending(ed);
		refresh(ed);
		return true;
	}
	return false;
}

const char *ti_editor_text(const struct ti_editor *ed)
{
	return ed->text;
}

const char *ti_editor_composing(const struct ti_editor *ed, int *typed)
{
	*typed = ed->nkeys;
	if (!ed->nkeys) {
		return "";
	}
	if (ed->ncands && ed->sel >= 0) {
		return ed->cand_text[ed->sel];
	}
	/* No word for the keys: show the digits. */
	return ed->keys;
}

const char *ti_editor_mode_label(const struct ti_editor *ed)
{
	static const char *const labels[3][3] = {
		{"T9 Abc", "T9 abc", "T9 ABC"},
		{"Abc", "abc", "ABC"},
		{"123", "123", "123"},
	};

	return labels[ed->mode][ed->shift];
}
