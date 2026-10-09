/*
 * Application-wide definitions: the UI event queue and app-level events.
 */

#pragma once

#include "ui/ui.h"

/* Event types handled by the main loop itself, outside the UI's range. */
enum app_event_type {
	APP_EV_LID = 0x40,  /* value 1 = lid closed */
	APP_EV_MESH = 0x41, /* value: enum mesh_event */
};

/* Queue an event for the UI thread. Safe to call from any thread. */
void app_post(const struct ui_event *ev);

/* Firmware version shown in the About screen. */
#define APP_VERSION "0.1.0"
