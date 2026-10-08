/*
 * Predictive text service: the language models (built into the firmware),
 * one prediction engine and user model per language, and the shared editor
 * of the compose screen.
 */

#pragma once

#include <stdbool.h>

#include "i18n.h"
#include "textinput/editor.h"

int textin_init(void);

struct ti_editor *textin_editor(void);

/* Input language (independent of the menu language). */
enum lang textin_lang(void);
void textin_set_lang(enum lang lang);
void textin_next_lang(void);

/* Store the learned words of both languages. */
void textin_save(void);
/* Forget all learned words. */
void textin_reset_learned(void);
