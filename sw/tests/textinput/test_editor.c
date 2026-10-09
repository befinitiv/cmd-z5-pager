/*
 * Host tests for lib/textinput with the real models in lm/.
 * Run: tests/textinput/run.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "textinput/editor.h"

static int failures;

#define CHECK(cond, ...)                                                                   \
	do {                                                                               \
		if (!(cond)) {                                                             \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
			printf(__VA_ARGS__);                                               \
			printf("\n");                                                      \
			failures++;                                                        \
		}                                                                          \
	} while (0)

#define CHECK_TEXT(ed, expected)                                                           \
	CHECK(strcmp(ti_editor_text(ed), expected) == 0, "text '%s', expected '%s'",       \
	      ti_editor_text(ed), expected)

static struct ti_model model;
static struct ti_user user;
static struct ti_engine eng;
static struct ti_editor ed;
static uint32_t now;

static void *load(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	void *data;

	if (!f) {
		perror(path);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = malloc(*size);
	if (fread(data, 1, *size, f) != *size) {
		exit(2);
	}
	fclose(f);
	return data;
}

/* Type a string of keys: digits, '*', '#', 'C'; '_' = pause (multitap timeout). */
static void type(const char *keys)
{
	for (; *keys; keys++) {
		now += 150;
		switch (*keys) {
		case '*':
			ti_editor_key(&ed, TI_EK_STAR, false, now);
			break;
		case '#':
			ti_editor_key(&ed, TI_EK_POUND, false, now);
			break;
		case 'C':
			ti_editor_key(&ed, TI_EK_CLEAR, false, now);
			break;
		case '_':
			now += TI_MULTITAP_MS;
			ti_editor_tick(&ed, now);
			break;
		default:
			ti_editor_key(&ed, *keys - '0', false, now);
			break;
		}
	}
}

static void reset(void)
{
	ti_user_init(&user);
	ti_engine_init(&eng, &model, &user);
	ti_editor_init(&ed, &eng, "de");
}

static void test_t9(void)
{
	int typed;

	reset();
	type("424");
	CHECK(strcmp(ti_editor_composing(&ed, &typed), "Ich") == 0, "composing '%s'",
	      ti_editor_composing(&ed, &typed));
	type("0");
	CHECK_TEXT(&ed, "Ich ");
	type("2460");
	CHECK_TEXT(&ed, "Ich bin ");

	/* Context decides between gut and Hut. */
	reset();
	type("6470");
	CHECK_TEXT(&ed, "Mir ");
	type("43480370488");
	CHECK(strcmp(ti_editor_composing(&ed, &typed), "gut") == 0, "after 'Mir geht es': '%s'",
	      ti_editor_composing(&ed, &typed));

	/* The wheel moves to the next candidate. */
	reset();
	type("488");
	ti_editor_wheel(&ed, -1);
	CHECK(ed.sel == 1, "sel %d", ed.sel);

	/* C removes keys, then characters. */
	reset();
	type("4240");
	type("2C");
	CHECK(ed.nkeys == 0, "nkeys %d", ed.nkeys);
	type("C");
	CHECK_TEXT(&ed, "Ich");
}

static void test_prediction(void)
{
	int typed;

	reset();
	type("4240");
	CHECK(ed.ncands > 0 && ed.sel == -1, "predictions %d sel %d", ed.ncands, ed.sel);
	/* Wheel down selects the first prediction, push accepts it. */
	ti_editor_wheel(&ed, -1);
	CHECK(ed.sel == 0, "sel %d", ed.sel);
	char expect[80];

	snprintf(expect, sizeof(expect), "Ich %s ", ed.cand_text[0]);
	ti_editor_select(&ed);
	CHECK_TEXT(&ed, expect);
	(void)typed;
}

static void test_punctuation(void)
{
	reset();
	type("42556");
	type("1");
	CHECK_TEXT(&ed, "Hallo.");
	type("1");
	CHECK_TEXT(&ed, "Hallo,");
	type("_0");
	CHECK_TEXT(&ed, "Hallo, ");
	/* Punctuation after an accepted word replaces the automatic space. */
	reset();
	type("425560");
	type("1");
	CHECK_TEXT(&ed, "Hallo.");
	/* After a sentence end the next word starts with a capital. */
	reset();
	type("42556");
	type("1_0");
	type("9430");
	CHECK_TEXT(&ed, "Hallo. Wie ");
}

static void test_multitap_and_learning(void)
{
	int typed;

	reset();
	/* Switch to multitap ("Abc") and spell an unknown name: X-a-v-e-r. */
	type("#");
	CHECK(ed.mode == TI_MODE_ABC, "mode %d", ed.mode);
	type("99_2_888_33_777_0");
	CHECK_TEXT(&ed, "Xaver ");
	/* Back to predictive mode: the name is offered now. */
	type("##");
	CHECK(ed.mode == TI_MODE_T9, "mode %d", ed.mode);
	type("92837");
	CHECK(strcmp(ti_editor_composing(&ed, &typed), "Xaver") == 0, "learned word: '%s'",
	      ti_editor_composing(&ed, &typed));

	/* Umlauts on the multitap keys. */
	reset();
	type("#2222_");
	CHECK_TEXT(&ed, "Ä");
}

static void test_long_press_and_123(void)
{
	reset();
	ti_editor_key(&ed, TI_EK_0 + 7, true, now);
	CHECK_TEXT(&ed, "7");
	type("##"); /* T9 -> abc -> 123 */
	CHECK(ed.mode == TI_MODE_123, "mode %d", ed.mode);
	type("42");
	CHECK_TEXT(&ed, "742");
	/* Long C deletes the last word. */
	ti_editor_key(&ed, TI_EK_CLEAR, true, now);
	CHECK_TEXT(&ed, "");
}

static void test_cursor(void)
{
	reset();
	type("4240"); /* "Ich " */
	type("2460"); /* "Ich bin " */
	CHECK(ti_editor_cursor(&ed) == 8, "cursor %d", ti_editor_cursor(&ed));

	/* '*' starts cursor mode; the wheel (up = back) moves it before "bin". */
	type("*");
	CHECK(ed.cursor_mode, "cursor mode off");
	ti_editor_wheel(&ed, 4);
	CHECK(ti_editor_cursor(&ed) == 4, "cursor %d", ti_editor_cursor(&ed));

	/* Typing inserts at the cursor and ends cursor mode. */
	type("4880"); /* "gut " */
	CHECK(!ed.cursor_mode, "cursor mode still on");
	CHECK_TEXT(&ed, "Ich gut bin ");
	CHECK(ti_editor_cursor(&ed) == 8, "cursor %d", ti_editor_cursor(&ed));

	/* C deletes before the cursor and stays in cursor mode. */
	type("*");
	ti_editor_wheel(&ed, 1);
	type("C");
	CHECK(ed.cursor_mode, "cursor mode left by C");
	CHECK_TEXT(&ed, "Ich gu bin ");
	CHECK(ti_editor_cursor(&ed) == 6, "cursor %d", ti_editor_cursor(&ed));

	/* The cursor stops at both ends; umlauts count as one character. */
	ti_editor_wheel(&ed, -100);
	CHECK(ti_editor_cursor(&ed) == (int)strlen(ed.text), "cursor %d", ti_editor_cursor(&ed));
	ti_editor_wheel(&ed, 100);
	CHECK(ti_editor_cursor(&ed) == 0, "cursor %d", ti_editor_cursor(&ed));

	/* At the start the context is the sentence start: capital letter. */
	type("*42556");
	CHECK(strcmp(ed.cand_text[0], "Hallo") == 0, "'%s'", ed.cand_text[0]);
	type("0");
	CHECK_TEXT(&ed, "Hallo Ich gu bin ");

	/* Multitap in the middle of the text. */
	reset();
	type("#2_2_");
	type("*");
	ti_editor_wheel(&ed, 1);
	type("*");
	type("33");
	CHECK_TEXT(&ed, "Aea");
}

static void test_english(void)
{
	static struct ti_model en;
	size_t size;
	void *data = load("lm/en.lm", &size);
	int typed;

	CHECK(ti_model_open(&en, data, size) == 0, "open en");
	ti_user_init(&user);
	ti_engine_init(&eng, &en, &user);
	ti_editor_init(&ed, &eng, "en");
	type("4");
	CHECK(strcmp(ti_editor_composing(&ed, &typed), "I") == 0, "'%s'",
	      ti_editor_composing(&ed, &typed));
	type("0");
	type("26");
	CHECK(strcmp(ti_editor_composing(&ed, &typed), "am") == 0, "'%s'",
	      ti_editor_composing(&ed, &typed));
}

int main(void)
{
	size_t size;
	void *data = load("lm/de.lm", &size);

	if (ti_model_open(&model, data, size) != 0) {
		printf("cannot open model\n");
		return 2;
	}
	test_t9();
	test_prediction();
	test_punctuation();
	test_multitap_and_learning();
	test_long_press_and_123();
	test_cursor();
	test_english();
	printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures,
	       failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
