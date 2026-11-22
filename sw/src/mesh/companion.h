/*
 * MeshCore companion protocol: the MeshCore app uses the device as its
 * radio, as with MeshCore's companion firmware (examples/companion_radio,
 * protocol version 12). The app writes one command frame at a time and
 * gets responses and pushes (frames starting with 0x80 and up) back. Over
 * Bluetooth (ble.c) a frame is one write or notification; the emulator
 * carries frames over TCP.
 *
 * The app and the device share contacts (the node database), channels,
 * settings and messages: direct messages sent from the app show in the
 * device's conversations too, and received messages are both stored and
 * queued for the app (up to COMPANION_QUEUE, fetched with "sync next
 * message"). Logins to repeaters and room servers, their commands, status,
 * telemetry and binary requests, path discovery and traces go between the
 * app and the mesh only (not stored). Commands run in the MeshCore thread;
 * what the device does not do is answered "unsupported".
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "meshcore/mesh.h"
#include "meshtastic/nodedb.h"

#define COMPANION_FRAME_MAX 176 /* MeshCore's MAX_FRAME_SIZE */
#define COMPANION_QUEUE     16  /* received messages kept for the app */

/* Transport side (any thread) -------------------------------------------------- */

/* The transport's wake-up: frames are waiting (companion_get()). */
void companion_attach(void (*wake)(void));
/* The app connected (and can receive) or left. */
void companion_connected(bool on);
bool companion_is_connected(void);
/* A command frame from the app. 0, or -EBUSY if it had to be dropped. */
int companion_put(const uint8_t *frame, size_t len);
/* The next frame for the app: its length, 0 if there is none. */
int companion_get(uint8_t buf[COMPANION_FRAME_MAX]);

/* MeshCore side (meshcore.c, MeshCore thread, mesh lock held) ----------------------- */

void companion_command(const uint8_t *frame, size_t len);
void companion_text(const uint8_t pub[32], uint32_t timestamp, const char *text,
		    const struct mc_rx_info *rx);
void companion_group(int channel, uint32_t timestamp, const char *text,
		     const struct mc_rx_info *rx);
void companion_advert(const struct mt_node *n, bool is_new);
void companion_path(const uint8_t pub[32]);
void companion_deleted(const uint8_t pub[32]);
void companion_response(const struct mt_node *n, const uint8_t *data, size_t len,
			const uint8_t *in_path, uint8_t in_path_len);
void companion_cli_text(const uint8_t pub[32], uint32_t timestamp, const char *text,
			const struct mc_rx_info *rx);
void companion_trace(uint32_t tag, uint32_t auth, uint8_t flags, const uint8_t *hashes,
		     uint8_t hashes_len, const int8_t *snrs, uint8_t snr_count, int8_t snr);
void companion_rx_log(const uint8_t *frame, size_t len, int16_t rssi, int8_t snr);
/* A message's status; true if it was the app's (then it reports it). */
bool companion_status(uint32_t id, enum mc_tx_status st);
