/*
 * MCUboot hooks for the CMD-Z5 (built into the bootloader only).
 *
 * In USB serial recovery MCUboot shows nothing and waits forever, so a
 * device that ended up there on battery looked dead until the cell was
 * disconnected. This module:
 *
 *  - shows a screen (boot/src/bootscreen.h, made with the device's fonts by
 *    tools/bootscreen/) while recovery runs: the display supply is the
 *    nPM1300's load switch 1, switched with one I2C write; the display bus
 *    is driven directly on the GPIO registers;
 *  - leaves recovery when it cannot be used: without USB power for 1 s, or
 *    with USB power but no host that gave the device an address within 15 s
 *    (a charger). It then switches the display off and restarts; the key is
 *    released by then, so MCUboot starts the application. With a PC
 *    connected recovery stays until an SMP reset, an update or unplugging.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

#include <nrfx.h>
#include <hal/nrf_gpio.h>

#include <bootutil/mcuboot_status.h>

#include "bootscreen.h"

#define NO_VBUS_MS   1000
#define NO_HOST_MS   15000
#define POLL_MS      100
/* The application's default contrast (src/config.c). */
#define CONTRAST     110

/* nPM1300: I2C address, load switch 1 on/off tasks (16-bit register address). */
#define PMIC_ADDR       0x6b
#define PMIC_LDSW_BASE  0x08
#define PMIC_LDSW1_SET  0x00
#define PMIC_LDSW1_CLR  0x01

/* Display bus pins (see the board devicetree, lcd-bus). */
#define LCD_D0    NRF_GPIO_PIN_MAP(0, 19) /* D0..D7 on P0.19..P0.26 */
#define LCD_WR    NRF_GPIO_PIN_MAP(1, 0)
#define LCD_A0    NRF_GPIO_PIN_MAP(0, 17)
#define LCD_RESET NRF_GPIO_PIN_MAP(1, 7)
#define LCD_DATA_MASK (0xffu << 19)

/* Display controller (see drivers/display/cmdz5_lcd.c). */
#define REG_CONTROL   0x00
#define REG_COLUMN    0x03
#define REG_ROW       0x04
#define REG_PIXELS    0x0B
#define REG_CONTRAST1 0x23
#define REG_CONTRAST2 0x24
#define CONTROL_OFF   0x00

static const struct device *const i2c = DEVICE_DT_GET(DT_BUS(DT_NODELABEL(pmic)));

/* Display ------------------------------------------------------------------- */

static void lcd_write(bool a0, const uint8_t *buf, size_t len)
{
	nrf_gpio_pin_write(LCD_A0, a0);
	for (size_t i = 0; i < len; i++) {
		NRF_P0->OUT = (NRF_P0->OUT & ~LCD_DATA_MASK) | ((uint32_t)buf[i] << 19);
		nrf_gpio_pin_clear(LCD_WR);
		/* The controller wants /WR low for about 300 ns. */
		for (int n = 0; n < 16; n++) {
			__NOP();
		}
		nrf_gpio_pin_set(LCD_WR);
	}
}

static void lcd_reg(uint8_t r, uint8_t v)
{
	lcd_write(false, &r, 1);
	lcd_write(true, &v, 1);
}

static void lcd_table(uint8_t r)
{
	static const uint8_t zero[16];

	lcd_write(false, &r, 1);
	lcd_write(true, zero, sizeof(zero));
}

/* The same sequence as drivers/display/cmdz5_lcd.c (replayed from the phone). */
static const uint8_t init_seq1[][2] = {
	{0x02, 0x01}, {0x00, 0x04}, {0x01, 0x00}, {0x05, 0x47}, {0x06, 0x27},
	{0x07, 0x11}, {0x08, 0x25}, {0x09, 0x00}, {0x0A, 0x00}, {0x0C, 0x00},
	{0x0D, 0x00}, {0x0E, 0x00}, {0x0F, 0x00},
};
static const uint8_t init_seq2[][2] = {
	{0x11, 0x00}, {0x12, 0x00}, {0x13, 0x00},
};
static const uint8_t init_seq3[][2] = {
	{0x15, 0x00}, {0x20, 0xDF}, {0x21, 0x52}, {0x22, 0x65},
	{0x23, 0x80}, {0x24, 0x80}, {0x25, 0x00}, {0x00, 0x00},
};
static const uint8_t on_seq[][2] = {
	{0x23, 0x7B}, {0x17, 0x00}, {0x18, 0x10}, {0x19, 0x18}, {0x1A, 0x20},
	{0x24, 0x7B}, {0x1B, 0x00}, {0x1C, 0x14}, {0x1D, 0x1A}, {0x1E, 0x20},
	{0x20, 0x1F}, {0x00, 0x40},
};

static void lcd_seq(const uint8_t (*seq)[2], size_t n)
{
	for (size_t i = 0; i < n; i++) {
		lcd_reg(seq[i][0], seq[i][1]);
	}
}

static int pmic_ldsw1(bool on)
{
	uint8_t buf[3] = {PMIC_LDSW_BASE, on ? PMIC_LDSW1_SET : PMIC_LDSW1_CLR, 1};

	return device_is_ready(i2c) ? i2c_write(i2c, buf, sizeof(buf), PMIC_ADDR) : -ENODEV;
}

static void screen_on(void)
{
	if (pmic_ldsw1(true) != 0) {
		return; /* no supply, no screen */
	}
	for (int i = 0; i < 8; i++) {
		nrf_gpio_cfg_output(LCD_D0 + i);
	}
	nrf_gpio_cfg_output(LCD_A0);
	nrf_gpio_cfg_output(LCD_WR);
	nrf_gpio_cfg_output(LCD_RESET);
	nrf_gpio_pin_set(LCD_WR);
	nrf_gpio_pin_clear(LCD_RESET);
	k_msleep(5); /* supply up, reset held */
	nrf_gpio_pin_set(LCD_RESET);
	k_msleep(5);

	lcd_seq(init_seq1, ARRAY_SIZE(init_seq1));
	lcd_table(0x10);
	lcd_seq(init_seq2, ARRAY_SIZE(init_seq2));
	lcd_table(0x14);
	lcd_seq(init_seq3, ARRAY_SIZE(init_seq3));
	for (int y = 0; y < (int)ARRAY_SIZE(bootscreen); y++) {
		lcd_reg(REG_COLUMN, 0);
		lcd_reg(REG_ROW, y);
		lcd_write(false, (const uint8_t[]){REG_PIXELS}, 1);
		lcd_write(true, bootscreen[y], sizeof(bootscreen[y]));
	}
	lcd_seq(on_seq, ARRAY_SIZE(on_seq));
	lcd_reg(REG_CONTRAST1, CONTRAST);
	lcd_reg(REG_CONTRAST2, CONTRAST);
}

static void screen_off(void)
{
	lcd_reg(REG_CONTROL, CONTROL_OFF);
	/* Parked as the application's driver leaves it: all lines low. */
	NRF_P0->OUT &= ~LCD_DATA_MASK;
	nrf_gpio_pin_clear(LCD_A0);
	nrf_gpio_pin_clear(LCD_WR);
	nrf_gpio_pin_clear(LCD_RESET);
	pmic_ldsw1(false);
}

/* Watch --------------------------------------------------------------------- */

static bool vbus(void)
{
	return NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk;
}

/* A host enumerated the device: it has a USB address. */
static bool host(void)
{
	return NRF_USBD->USBADDR != 0;
}

static void watch(void *p1, void *p2, void *p3)
{
	int no_vbus = 0, no_host = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	k_sleep(K_FOREVER); /* woken when recovery starts */
	for (;;) {
		k_msleep(POLL_MS);
		no_vbus = vbus() ? 0 : no_vbus + POLL_MS;
		no_host = host() || !vbus() ? 0 : no_host + POLL_MS;
		if (no_vbus >= NO_VBUS_MS || no_host >= NO_HOST_MS) {
			screen_off();
			sys_reboot(SYS_REBOOT_COLD);
		}
	}
}

K_THREAD_DEFINE(boot_watch, 1024, watch, NULL, NULL, NULL, K_PRIO_PREEMPT(5), 0, 0);

void mcuboot_status_change(mcuboot_status_type_t status)
{
	if (status == MCUBOOT_STATUS_SERIAL_DFU_ENTERED) {
		screen_on();
		k_wakeup(boot_watch);
	}
}
