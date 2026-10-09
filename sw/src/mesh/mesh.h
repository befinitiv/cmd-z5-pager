/*
 * Mesh service: runs the Meshtastic router in its own thread, keeps the
 * device identity (node number, X25519 key pair, names), the node database
 * and the messages, and notifies the UI (APP_EV_MESH).
 *
 * Lock order: the mesh lock before the message store lock. Do not call mesh
 * functions while holding the message store lock.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "meshtastic/nodedb.h"

/* Values of the APP_EV_MESH event. */
enum mesh_event {
	MESH_EV_MESSAGE = 1, /* a new text message */
	MESH_EV_STATUS,      /* delivery status of a sent message changed */
	MESH_EV_NODES,       /* node database changed */
};

struct mesh_config {
	uint8_t region; /* enum mt_region */
	uint8_t preset; /* enum mt_preset */
	char long_name[40];
	char short_name[5];
};

int mesh_init(void);
/* false if the board has no (supported) radio yet. */
bool mesh_available(void);
uint32_t mesh_my_num(void);
const char *mesh_channel_name(void);

void mesh_get_config(struct mesh_config *c);
/* Apply (radio settings, names) and store. */
void mesh_set_config(const struct mesh_config *c);

/* Store and send a text; to = MT_BROADCAST for the channel. 0 or -errno. */
int mesh_send_text(uint32_t to, const char *text);

/* Node database access from other threads: lock around it. */
void mesh_lock(void);
void mesh_unlock(void);
struct mt_nodedb *mesh_nodes(void);
/* Display name of a node (copied into buf). */
const char *mesh_node_name(uint32_t num, char *buf, int size);
/* Uptime in seconds, the time base of mt_node.last_heard. */
uint32_t mesh_uptime_s(void);
