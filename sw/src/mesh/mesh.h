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
#include "msgstore.h"

/* Values of the APP_EV_MESH event. */
enum mesh_event {
	MESH_EV_MESSAGE = 1, /* a new text message */
	MESH_EV_STATUS,      /* delivery status of a sent message changed */
	MESH_EV_NODES,       /* node database changed */
	MESH_EV_FAILED,      /* a sent message failed, or nobody relayed it */
};

struct mesh_config {
	uint8_t region; /* enum mt_region */
	uint8_t preset; /* enum mt_preset */
	char long_name[40];
	char short_name[5];
};

/*
 * Private channels in slots 1..MESH_CHANNELS-1 (slot 0 is the primary
 * channel: the preset's name and the default key). They share the primary
 * channel's frequency and preset; each has its own conversation
 * (conv_of_channel()).
 *
 * Key types:
 *   number      Meshtastic's 1-byte key: the default key with its last byte
 *               changed. Easy to set up on both sides, but only 255 values:
 *               separates groups, does not keep anything secret.
 *   passphrase  An AES-128 key from SHA-256 of the passphrase; the Meshtastic
 *               app gets it from the channel's QR code (mesh_channel_url()).
 */
#define MESH_CHANNELS     CONV_CHANNELS
#define MESH_CHANNEL_NAME 11 /* Meshtastic's limit */
#define MESH_KEY_LEN      16

enum mesh_key_type {
	MESH_KEY_NUMBER,
	MESH_KEY_PASSPHRASE,
};

struct mesh_channel {
	char name[MESH_CHANNEL_NAME + 1]; /* "" = slot unused */
	uint8_t key_type;                 /* enum mesh_key_type */
	uint8_t number;                   /* MESH_KEY_NUMBER: 1..255 */
	uint8_t key[MESH_KEY_LEN];        /* MESH_KEY_PASSPHRASE */
};

/* Slot 1..MESH_CHANNELS-1; false if unused. */
bool mesh_get_channel(int slot, struct mesh_channel *c);
/* Store and apply; c = NULL deletes the channel and its messages. */
void mesh_set_channel(int slot, const struct mesh_channel *c);
/* A free slot, or -1. */
int mesh_free_channel(void);
/* "LongFast" for slot 0, the name of a private channel, or "". */
void mesh_channel_label(int slot, char *buf, size_t size);
/* Name of a conversation: "#LongFast", "#Family", or the node's name. */
void mesh_conv_name(uint32_t conv, char *buf, size_t size);
/* Key from a passphrase (case, extra spaces ignored). */
void mesh_key_from_passphrase(const char *phrase, uint8_t key[MESH_KEY_LEN]);
/*
 * Meshtastic link that adds the channel in the app ("?add=true": the other
 * channels and the LoRa settings stay). Returns the length, or -1.
 */
int mesh_channel_url(int slot, char *buf, size_t size);

int mesh_init(void);
/* false if the board has no (supported) radio yet. */
bool mesh_available(void);
uint32_t mesh_my_num(void);
const char *mesh_channel_name(void);

void mesh_get_config(struct mesh_config *c);
/* Apply (radio settings, names) and store. */
void mesh_set_config(const struct mesh_config *c);

/*
 * Rebroadcast other nodes' packets (managed flooding). Stored, default on;
 * off is Meshtastic's CLIENT_MUTE role: receive and send, but never relay.
 */
bool mesh_relay(void);
void mesh_set_relay(bool on);

/* Counters since the start. */
struct mesh_stats {
	uint32_t rx;        /* frames received, duplicates included */
	uint32_t tx;        /* frames sent: own packets, retries and rebroadcasts */
	uint32_t relayed;   /* rebroadcasts sent */
	uint32_t cancelled; /* rebroadcasts dropped: another node relayed first */
};
void mesh_get_stats(struct mesh_stats *s);

/*
 * Our position or the sharing setting changed (from the GPS service):
 * broadcast it if it is the first, sharing changed, or we moved far enough.
 */
void mesh_position_update(void);

/* Store and send a text to a conversation (a node or a channel). 0 or -errno. */
int mesh_send_text(uint32_t conv, const char *text);

/* Node database access from other threads: lock around it. */
void mesh_lock(void);
void mesh_unlock(void);
struct mt_nodedb *mesh_nodes(void);
/* Display name of a node (copied into buf). */
const char *mesh_node_name(uint32_t num, char *buf, int size);
/* Uptime in seconds, the time base of mt_node.last_heard. */
uint32_t mesh_uptime_s(void);
