/*
 * Scripted input for the emulator: --script=<file> runs one command per line,
 * for UI tests and screenshots without a person at the keyboard. With
 * SDL_VIDEODRIVER=offscreen it also runs without a visible window.
 *
 *   wait <ms>               sleep
 *   key <name> [<ms>]       press, hold (default 100 ms), release
 *   press <name>            press and keep pressed
 *   release <name>          release
 *   wheel <detents>         rotate, positive = up
 *   lid open|closed
 *   usb on|off              USB power for the simulated battery
 *   shot <file.bmp>         save the whole window after the next frame
 *   quit                    exit the emulator
 *
 * Key names: 0-9, star, pound, call, memo, clear, push, up, down, power.
 * Lines starting with '#' are comments.
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cmdline.h>
#include <posix_board_if.h>
#include <posix_native_task.h>

#include "frontpanel_bottom.h"

LOG_MODULE_REGISTER(fp_script, CONFIG_CMDZ5_EMUL_LOG_LEVEL);

#define SCRIPT_MAX 8192

static char *script_path;
static char script[SCRIPT_MAX];

K_SEM_DEFINE(frontpanel_ready, 0, 1);

static const struct {
	const char *name;
	enum fp_button button;
} names[] = {
	{"1", FP_BTN_1},         {"2", FP_BTN_2},          {"3", FP_BTN_3},
	{"4", FP_BTN_4},         {"5", FP_BTN_5},          {"6", FP_BTN_6},
	{"7", FP_BTN_7},         {"8", FP_BTN_8},          {"9", FP_BTN_9},
	{"0", FP_BTN_0},         {"star", FP_BTN_STAR},    {"pound", FP_BTN_POUND},
	{"call", FP_BTN_CALL},   {"memo", FP_BTN_MEMO},    {"clear", FP_BTN_CLEAR},
	{"push", FP_BTN_WHEEL_PUSH}, {"up", FP_BTN_WHEEL_UP}, {"down", FP_BTN_WHEEL_DOWN},
	{"power", FP_BTN_POWER},
};

static int button_by_name(const char *name)
{
	for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
		if (strcmp(names[i].name, name) == 0) {
			return names[i].button;
		}
	}
	return -1;
}

static void inject(uint8_t type, int button, int32_t value)
{
	struct fp_event ev = {.type = type, .button = button, .value = value};

	fp_bottom_inject(&ev);
}

/* Split off the next whitespace-separated word; NULL at the end of the line. */
static char *next_word(char **p)
{
	char *s = *p;

	while (*s == ' ' || *s == '\t' || *s == '\r') {
		s++;
	}
	if (*s == '\0') {
		*p = s;
		return NULL;
	}
	char *word = s;

	while (*s && *s != ' ' && *s != '\t' && *s != '\r') {
		s++;
	}
	if (*s) {
		*s++ = '\0';
	}
	*p = s;
	return word;
}

static void run_line(int lineno, char *line)
{
	char *cmd = next_word(&line);
	char *arg1 = next_word(&line);
	char *arg2 = next_word(&line);

	if (!cmd || cmd[0] == '#') {
		return;
	}

	if (strcmp(cmd, "wait") == 0 && arg1) {
		k_msleep(atoi(arg1));
	} else if (strcmp(cmd, "key") == 0 || strcmp(cmd, "press") == 0 ||
		   strcmp(cmd, "release") == 0) {
		int b = arg1 ? button_by_name(arg1) : -1;

		if (b < 0) {
			LOG_ERR("line %d: unknown key '%s'", lineno, arg1 ? arg1 : "");
			return;
		}
		if (cmd[0] != 'r') {
			inject(FP_EV_BUTTON, b, 1);
		}
		if (cmd[0] == 'k') {
			k_msleep(arg2 ? atoi(arg2) : 100);
		}
		if (cmd[0] != 'p') {
			inject(FP_EV_BUTTON, b, 0);
		}
	} else if (strcmp(cmd, "wheel") == 0 && arg1) {
		inject(FP_EV_WHEEL, 0, atoi(arg1));
	} else if (strcmp(cmd, "lid") == 0 && arg1) {
		inject(FP_EV_LID, 0, strcmp(arg1, "closed") == 0);
	} else if (strcmp(cmd, "usb") == 0 && arg1) {
		inject(FP_EV_USB, 0, strcmp(arg1, "on") == 0);
	} else if (strcmp(cmd, "shot") == 0 && arg1) {
		fp_bottom_request_shot(arg1);
		while (fp_bottom_shot_pending()) {
			k_msleep(10);
		}
	} else if (strcmp(cmd, "quit") == 0) {
		posix_exit(0);
	} else {
		LOG_ERR("line %d: cannot parse '%s'", lineno, cmd);
	}
}

static void script_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (!script_path) {
		return;
	}
	if (fp_bottom_read_file(script_path, script, sizeof(script)) < 0) {
		LOG_ERR("cannot read script %s", script_path);
		return;
	}
	k_sem_take(&frontpanel_ready, K_FOREVER);

	char *line = script;

	for (int lineno = 1; line && *line; lineno++) {
		char *end = strchr(line, '\n');

		if (end) {
			*end = '\0';
		}
		run_line(lineno, line);
		line = end ? end + 1 : NULL;
	}
}

K_THREAD_DEFINE(frontpanel_script, CONFIG_ARCH_POSIX_RECOMMENDED_STACK_SIZE, script_thread,
		NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

static void add_options(void)
{
	static struct args_struct_t options[] = {
		{
			.option = "script",
			.name = "file",
			.type = 's',
			.dest = (void *)&script_path,
			.descript = "Run front panel commands from a file (keys, wheel, lid, "
				    "screenshots), see emul/script.c",
		},
		ARG_TABLE_ENDMARKER,
	};

	native_add_command_line_opts(options);
}

NATIVE_TASK(add_options, PRE_BOOT_1, 10);
