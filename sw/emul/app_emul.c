/*
 * The MeshCore app's link in the emulator: TCP on localhost, framed as
 * MeshCore's companion firmware does over TCP and serial ('<', 16-bit
 * little-endian length, frame from the app; '>' ... to the app). The
 * meshcore Python library and meshcore-cli connect to it.
 *
 * Option: --app-port=<n> (off without it). MeshCore mode only.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <cmdline.h>
#include <posix_native_task.h>

#include "../src/mesh/companion.h"
#include "app_bottom.h"
#include "emul.h"

LOG_MODULE_REGISTER(app_emul, CONFIG_CMDZ5_EMUL_LOG_LEVEL);

#define POLL_MS 5

static uint32_t opt_port = UINT32_MAX;

/* Frame assembly: header ('<' and the length), then the frame. */
static uint8_t in[3 + COMPANION_FRAME_MAX];
static size_t in_len, want;

static void receive(const uint8_t *data, int n)
{
	for (int i = 0; i < n; i++) {
		if (in_len == 0 && data[i] != '<') {
			continue; /* resynchronise */
		}
		in[in_len++] = data[i];
		if (in_len == 3) {
			want = sys_get_le16(in + 1);
			if (want == 0 || want > COMPANION_FRAME_MAX) {
				in_len = 0;
			}
		} else if (in_len > 3 && in_len == 3 + want) {
			if (companion_put(in + 3, want) != 0) {
				LOG_WRN("app frame dropped");
			}
			in_len = 0;
		}
	}
}

static void app_thread(void *p1, void *p2, void *p3)
{
	uint8_t buf[3 + COMPANION_FRAME_MAX];
	bool connected = false;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (;;) {
		int n;

		k_sleep(K_MSEC(POLL_MS));
		if (app_bottom_accept()) {
			LOG_INF("app client connected");
			in_len = 0;
			connected = true;
			companion_connected(true);
		}
		if (!connected) {
			continue;
		}
		while ((n = app_bottom_recv(buf, sizeof(buf))) > 0) {
			receive(buf, n);
		}
		while (n >= 0 && (n = companion_get(buf + 3)) > 0) {
			buf[0] = '>';
			sys_put_le16(n, buf + 1);
			n = app_bottom_send(buf, 3 + n);
		}
		if (n < 0) {
			LOG_INF("app client left");
			connected = false;
			companion_connected(false);
		}
	}
}

K_THREAD_STACK_DEFINE(app_stack, 2048);
static struct k_thread app_tid;

static int app_emul_init(void)
{
	if (opt_port == UINT32_MAX || opt_port == 0 || !emul_meshcore()) {
		return 0;
	}
	if (app_bottom_listen(opt_port) != 0) {
		LOG_WRN("app port %u not available", opt_port);
		return 0;
	}
	LOG_INF("MeshCore app link on TCP port %u", opt_port);
	k_thread_create(&app_tid, app_stack, K_THREAD_STACK_SIZEOF(app_stack), app_thread, NULL,
			NULL, NULL, K_PRIO_PREEMPT(8), 0, K_MSEC(500));
	k_thread_name_set(&app_tid, "app_link");
	return 0;
}

SYS_INIT(app_emul_init, APPLICATION, 99);

static void add_options(void)
{
	static struct args_struct_t options[] = {
		{.option = "app-port", .name = "port", .type = 'u', .dest = &opt_port,
		 .descript = "TCP port for the MeshCore app link (MeshCore mode)"},
		ARG_TABLE_ENDMARKER,
	};

	native_add_command_line_opts(options);
}

NATIVE_TASK(add_options, PRE_BOOT_1, 14);
