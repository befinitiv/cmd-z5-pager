/*
 * Text entry state machine for the phone keypad: predictive (T9 with
 * completions and next-word prediction), multitap and numeric input, a
 * symbol picker, case modes and learning of typed words.
 *
 * Text is inserted and deleted at the cursor. In cursor mode ('*') the wheel
 * moves the cursor instead of selecting candidates. Keys are given as short
 * or long presses (short = released before the long press time). Plain C,
 * no Zephyr dependency.
 *
 * Keys in predictive mode:
 *   2-9    add a key to the word        long: the digit
 *   1      ends the word; punctuation, press again for the next one
 *          (the bar lists the choices; the wheel also steps, push accepts)
 *   0      accept the word and add a space   long: '0'
 *   *      cursor mode on/off: the wheel moves the cursor   long: case Abc / abc / ABC
 *   #      mode T9 -> abc -> 123        long: language (callback)
 *   C      delete key/char before the cursor   long: delete the word before it
 *   wheel  move through the candidates; push accepts (with a space)
 *
 * Typing ends cursor mode; the cursor stays where it was placed.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "textinput/predict.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TI_TEXT_MAX     200
#define TI_EDIT_CANDS   8
#define TI_MULTITAP_MS  1000

enum ti_mode {
	TI_MODE_T9,
	TI_MODE_ABC,
	TI_MODE_123,
};

enum ti_shift {
	TI_SHIFT_AUTO,  /* "Abc": capital letter at the start of a sentence */
	TI_SHIFT_LOWER, /* "abc": as typed / as in the lexicon */
	TI_SHIFT_UPPER, /* "ABC" */
};

enum ti_ekey {
	TI_EK_0 = 0, /* ... TI_EK_0 + 9 for the digits */
	TI_EK_STAR = 10,
	TI_EK_POUND = 11,
	TI_EK_CLEAR = 12,
};

struct ti_editor {
	struct ti_engine *eng;
	const char *const *multitap; /* characters per key '0'..'9' */
	void (*lang_cb)(void *arg);  /* long '#' */
	void *lang_arg;
	bool learn;

	char text[TI_TEXT_MAX + 1];
	uint16_t len;
	uint16_t cursor; /* byte offset; text is inserted here */
	bool cursor_mode; /* the wheel moves the cursor */
	enum ti_mode mode;
	enum ti_shift shift;

	/* Predictive composition (keys typed for the current word). */
	char keys[TI_MAX_KEYS + 1];
	uint8_t nkeys;
	/* Candidates: for the keys, or next-word predictions if nkeys == 0. */
	struct ti_cand cands[TI_EDIT_CANDS];
	char cand_text[TI_EDIT_CANDS][TI_WORD_BYTES];
	uint8_t ncands;
	int8_t sel; /* -1: no candidate selected (predictions only) */

	/* Multitap: the last pend_len bytes of text are still changeable. */
	char pend_key;
	uint8_t pend_idx;
	uint8_t pend_len;
	uint32_t pend_time;

	/* Symbol picker. */
	bool symbols;
	uint8_t sym_sel;

	/* Context of the next word. */
	int32_t p2, p1;
};

/* Multitap tables for a language code ("de", "en"). */
const char *const *ti_multitap_table(const char *lang);

/* eng may be NULL (no language model): multitap and 123 only. */
void ti_editor_init(struct ti_editor *ed, struct ti_engine *eng, const char *lang);
/* Switch the language (engine with another model); keeps the text. */
void ti_editor_set_engine(struct ti_editor *ed, struct ti_engine *eng, const char *lang);
void ti_editor_clear(struct ti_editor *ed);
/* Accept the word being composed (no space) and a pending multitap character. */
void ti_editor_commit(struct ti_editor *ed);
/* Replace the text (cursor at the end). */
void ti_editor_set_text(struct ti_editor *ed, const char *text);

/* Input. Each returns true if the editor used the input. */
bool ti_editor_key(struct ti_editor *ed, enum ti_ekey key, bool long_press, uint32_t now_ms);
/* now_ms: a pending key 1 symbol changed by the wheel stays open TI_MULTITAP_MS longer. */
bool ti_editor_wheel(struct ti_editor *ed, int delta, uint32_t now_ms);
bool ti_editor_select(struct ti_editor *ed);
/* Switch cursor mode (the wheel moves the cursor) on or off. */
void ti_editor_cursor_mode(struct ti_editor *ed, bool on);
/* Open or close the symbol picker. */
void ti_editor_symbols(struct ti_editor *ed, bool open);
/* Finish a multitap character after its timeout. Returns true on change. */
bool ti_editor_tick(struct ti_editor *ed, uint32_t now_ms);

/* Committed text (without the word being composed). */
const char *ti_editor_text(const struct ti_editor *ed);
/* Cursor position in the committed text (byte offset). */
int ti_editor_cursor(const struct ti_editor *ed);
/*
 * Word being composed (predictive mode), "" if none. *typed is set to the
 * number of characters covered by the typed keys; the rest is a completion.
 */
const char *ti_editor_composing(const struct ti_editor *ed, int *typed);
/* Short mode label: "T9 Abc", "abc", "123", ... */
const char *ti_editor_mode_label(const struct ti_editor *ed);

/*
 * While a punctuation character of key 1 is pending: its choices (UTF-8,
 * one character each) for the bar, *sel = the current one. Returns the
 * number of choices, 0 if none is pending. The wheel steps through them,
 * push accepts.
 */
int ti_editor_pending_symbols(const struct ti_editor *ed, char out[][5], int max, int *sel);

/* Symbol picker contents. */
int ti_editor_symbol_count(void);
const char *ti_editor_symbol(int i);

#ifdef __cplusplus
}
#endif
