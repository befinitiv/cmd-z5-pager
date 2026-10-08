#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "config.h"
#include "textin.h"

LOG_MODULE_REGISTER(textin, CONFIG_CMDZ5_APP_LOG_LEVEL);

/*
 * The models are linked into the application for now. Later they move to the
 * lm flash partition, so they can be updated separately.
 */
static const uint8_t model_de[] = {
#include "lm/de.lm.inc"
};
static const uint8_t model_en[] = {
#include "lm/en.lm.inc"
};

struct language {
	const char *code;
	const uint8_t *data;
	size_t size;
	struct ti_model model;
	struct ti_user user;
	struct ti_engine engine;
	bool ok;
};

static struct language langs[LANG_COUNT] = {
	[LANG_EN] = {.code = "en", .data = model_en, .size = sizeof(model_en)},
	[LANG_DE] = {.code = "de", .data = model_de, .size = sizeof(model_de)},
};

static struct ti_editor editor;
static enum lang current;

static int user_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	for (int i = 0; i < LANG_COUNT; i++) {
		struct ti_user *u = &langs[i].user;

		if (strncmp(name, "user_", 5) == 0 && strcmp(name + 5, langs[i].code) == 0) {
			if (len != sizeof(*u) || read_cb(cb_arg, u, len) != (ssize_t)len ||
			    !ti_user_valid(u)) {
				ti_user_init(u);
			}
			return 0;
		}
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(textin, "ti", NULL, user_set, NULL, NULL);

static void lang_switch(void *arg)
{
	ARG_UNUSED(arg);
	textin_next_lang();
}

int textin_init(void)
{
	for (int i = 0; i < LANG_COUNT; i++) {
		struct language *l = &langs[i];

		ti_user_init(&l->user);
		l->ok = ti_model_open(&l->model, l->data, l->size) == 0;
		if (!l->ok) {
			LOG_ERR("language model %s is invalid", l->code);
			continue;
		}
		LOG_INF("language model %s: %u words, %u bytes", l->code, l->model.V, l->size);
	}
	settings_load_subtree("ti");
	for (int i = 0; i < LANG_COUNT; i++) {
		ti_engine_init(&langs[i].engine, &langs[i].model, &langs[i].user);
	}

	current = app_config.input_lang < LANG_COUNT ? app_config.input_lang : LANG_DE;
	if (!langs[current].ok) {
		return -EINVAL;
	}
	ti_editor_init(&editor, &langs[current].engine, langs[current].code);
	editor.lang_cb = lang_switch;
	editor.learn = app_config.learn_words;
	return 0;
}

struct ti_editor *textin_editor(void)
{
	editor.learn = app_config.learn_words;
	return &editor;
}

enum lang textin_lang(void)
{
	return current;
}

void textin_set_lang(enum lang lang)
{
	if (lang >= LANG_COUNT || !langs[lang].ok) {
		return;
	}
	current = lang;
	ti_editor_set_engine(&editor, &langs[lang].engine, langs[lang].code);
	if (app_config.input_lang != lang) {
		app_config.input_lang = lang;
		config_save();
	}
}

void textin_next_lang(void)
{
	textin_set_lang((current + 1) % LANG_COUNT);
}

void textin_save(void)
{
	char key[16];

	for (int i = 0; i < LANG_COUNT; i++) {
		snprintf(key, sizeof(key), "ti/user_%s", langs[i].code);
		int ret = settings_save_one(key, &langs[i].user, sizeof(langs[i].user));

		if (ret) {
			LOG_WRN("saving %s failed (%d)", key, ret);
		}
	}
}

void textin_reset_learned(void)
{
	for (int i = 0; i < LANG_COUNT; i++) {
		ti_user_init(&langs[i].user);
	}
	textin_save();
}
