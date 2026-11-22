/*
 * Bluetooth LE: the MeshCore app's link, as with MeshCore's companion
 * firmware. Nordic UART service: the app writes command frames to the RX
 * characteristic and gets responses as notifications of the TX
 * characteristic (src/mesh/companion.c speaks the protocol). Advertised as
 * "MeshCore-<name>".
 *
 * Phones pair with a passkey shown on the display (the device is "display
 * only"); the service needs an encrypted, authenticated link, so only a
 * paired phone can use it. Bonds are stored (up to 4, the oldest replaced).
 *
 * MeshCore mode only; switched in Settings > Mesh > Bluetooth (stored,
 * default off). The emulator has no Bluetooth (its app link is TCP).
 */

#pragma once

#include <stdbool.h>

/* Values of the APP_EV_BLE event. */
enum ble_event {
	BLE_EV_PASSKEY = 1, /* a phone pairs: show ble_passkey() */
	BLE_EV_PAIRED,
	BLE_EV_PAIR_FAILED,
	BLE_EV_CONNECTION, /* connected or disconnected */
};

/* Start if switched on (after mesh_init()). */
void ble_init(void);
/* Built with Bluetooth and in MeshCore mode. */
bool ble_available(void);
bool ble_enabled(void);
/* Switch on or off (stored); off drops a connection. */
void ble_set_enabled(bool on);
/* An app is connected. */
bool ble_connected(void);
/* The passkey to show while a phone pairs, or -1. */
int ble_passkey(void);
void ble_pairing_cancel(void);
/* Forget all paired phones (and drop a connection). */
void ble_forget(void);
