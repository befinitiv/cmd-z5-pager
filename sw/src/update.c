#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>

#include "update.h"

#if defined(CONFIG_BOOTLOADER_MCUBOOT) && defined(CONFIG_RETENTION_BOOT_MODE)
#include <zephyr/retention/bootmode.h>
#define HAVE_BOOTLOADER 1
#else
#define HAVE_BOOTLOADER 0
#endif

LOG_MODULE_REGISTER(update, CONFIG_CMDZ5_APP_LOG_LEVEL);

/*
 * The host asks for the bootloader by setting the USB serial port to
 * 1200 baud, as for Arduino and Meshtastic nRF52 boards. The port reports
 * line coding changes only through its state, so it is polled.
 */
#define TOUCH_BAUD    1200
#define TOUCH_POLL_MS 500

#if HAVE_BOOTLOADER && DT_HAS_CHOSEN(zephyr_console) && \
	DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_console), zephyr_cdc_acm_uart) && \
	defined(CONFIG_UART_LINE_CTRL)
#define HAVE_TOUCH 1
static const struct device *const port = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static struct k_work_delayable touch_work;

static void touch_poll(struct k_work *work)
{
	uint32_t baud;

	if (uart_line_ctrl_get(port, UART_LINE_CTRL_BAUD_RATE, &baud) == 0 &&
	    baud == TOUCH_BAUD) {
		LOG_INF("1200-baud touch");
		update_enter_bootloader();
	}
	k_work_schedule(k_work_delayable_from_work(work), K_MSEC(TOUCH_POLL_MS));
}
#else
#define HAVE_TOUCH 0
#endif

void update_init(void)
{
#if HAVE_TOUCH
	k_work_init_delayable(&touch_work, touch_poll);
	k_work_schedule(&touch_work, K_MSEC(TOUCH_POLL_MS));
#endif
}

bool update_available(void)
{
	return HAVE_BOOTLOADER;
}

void update_enter_bootloader(void)
{
#if HAVE_BOOTLOADER
	int ret = bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);

	if (ret) {
		LOG_ERR("boot mode not set (%d)", ret);
		return;
	}
	LOG_WRN("restarting into the bootloader");
	log_flush();
	sys_reboot(SYS_REBOOT_WARM);
#else
	LOG_WRN("no bootloader");
#endif
}
