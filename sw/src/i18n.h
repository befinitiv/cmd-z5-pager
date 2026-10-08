/*
 * Menu language. Strings in the code are English and are translated when
 * drawn: tr("Settings"). Strings without a translation stay English.
 */

#pragma once

enum lang {
	LANG_EN,
	LANG_DE,
	LANG_COUNT,
};

void i18n_set(enum lang lang);
enum lang i18n_get(void);

/* Translation of an English UI string into the current language. */
const char *tr(const char *en);

/* Name of a language in that language, e.g. "Deutsch". */
const char *i18n_name(enum lang lang);
