/*
 * Display and backlight power states, driven from the UI thread.
 *
 * ACTIVE: display and backlight on.
 * DIM:    display on, backlight off (after the backlight timeout).
 * OFF:    display off (after the screen timeout or power key).
 *
 * Closing the lid covers only the keypad: the display stays visible, so the
 * UI returns to the home screen and the backlight goes off. Keypad keys are
 * ignored while the lid is closed; the side controls (wheel, power key) work.
 *
 * The radio and the low-power mode come in later phases.
 */

#pragma once

#include <stdbool.h>

void power_init(void);

/*
 * Report user input. keypad: the input comes from a key under the lid.
 * Returns true if the input must not be passed on to the UI, because it only
 * woke the display or the key is covered by the closed lid.
 */
bool power_user_activity(bool keypad);

void power_lid(bool closed);
/* Power key short press: switch the display off, or on if it is off. */
void power_toggle_display(void);
/* Apply changed settings (timeouts, backlight level). */
void power_settings_changed(void);
/* Check the timeouts; call regularly. */
void power_update(void);

bool power_display_on(void);
