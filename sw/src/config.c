#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "config.h"
#include "i18n.h"

LOG_MODULE_REGISTER(config, CONFIG_CMDZ5_APP_LOG_LEVEL);

/*
 * Stored as one blob under "cfg/<CONFIG_VERSION>". Bump the version when the
 * layout changes; older blobs are then ignored and the defaults apply.
 */
#define CONFIG_VERSION "v3"
#define CONFIG_KEY     "cfg/" CONFIG_VERSION

static const struct app_config defaults = {
	.contrast = 0x85,
	.backlight_level = 100,
	.backlight_timeout_s = 10,
	.screen_timeout_s = 0, /* never */
	.volume = 60,
	.key_click = false,
	.vibrate = true,
	.language = LANG_EN,
	.input_lang = LANG_DE,
	.learn_words = true,
};

struct app_config app_config;

#ifdef CONFIG_SETTINGS
static int config_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, CONFIG_VERSION) != 0 || len != sizeof(app_config)) {
		return -ENOENT;
	}
	return read_cb(cb_arg, &app_config, sizeof(app_config)) == sizeof(app_config) ? 0 : -EIO;
}

SETTINGS_STATIC_HANDLER_DEFINE(cfg, "cfg", NULL, config_set, NULL, NULL);
#endif

void config_init(void)
{
	app_config = defaults;
#ifdef CONFIG_SETTINGS
	int ret = settings_subsys_init();

	if (ret == 0) {
		ret = settings_load_subtree("cfg");
	}
	if (ret) {
		LOG_WRN("settings not available (%d), using defaults", ret);
	}
#endif
}

void config_save(void)
{
#ifdef CONFIG_SETTINGS
	int ret = settings_save_one(CONFIG_KEY, &app_config, sizeof(app_config));

	if (ret) {
		LOG_WRN("saving settings failed (%d)", ret);
	}
#endif
}
