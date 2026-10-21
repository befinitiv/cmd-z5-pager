/*
 * Firmware update over USB: restart into MCUboot serial recovery. Either from
 * the menu (Settings -> Firmware update) or from the host with the 1200-baud
 * touch: open the USB serial port at 1200 baud (`stty -F /dev/ttyACM0 1200`).
 */

#pragma once

#include <stdbool.h>

/* Watch the USB serial port for the 1200-baud touch. */
void update_init(void);

/* False without a bootloader (emulator, build without sysbuild). */
bool update_available(void);

/* Restart into the bootloader; returns only if that is not possible. */
void update_enter_bootloader(void);
