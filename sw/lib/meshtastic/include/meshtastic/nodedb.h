/*
 * Known nodes: names, public keys, position and radio statistics. Fixed size;
 * the least recently heard node is replaced when the table is full.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "meshtastic/packet.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MT_MAX_NODES 48

struct mt_node {
	uint32_t num;
	bool has_user;
	struct mt_user user;
	bool has_position;
	struct mt_position position;
	uint32_t last_heard; /* seconds (wall clock if known, else uptime) */
	int16_t rssi;
	int8_t snr;
	uint8_t hops_away;   /* 0 = direct */
	/* Cached PKI key (SHA-256 of the X25519 secret), see the router. */
	bool has_shared_key;
	uint8_t shared_key[32];
};

struct mt_nodedb {
	struct mt_node nodes[MT_MAX_NODES];
	int count;
};

void mt_nodedb_init(struct mt_nodedb *db);
struct mt_node *mt_nodedb_find(struct mt_nodedb *db, uint32_t num);
/* Find or create (replacing the least recently heard node). */
struct mt_node *mt_nodedb_get(struct mt_nodedb *db, uint32_t num, uint32_t now);
/* Set a node's user info; invalidates the cached key if the public key changed. */
void mt_nodedb_set_user(struct mt_node *n, const struct mt_user *u);

/* Indexes of the nodes sorted by last heard, newest first. Returns the count. */
int mt_nodedb_sorted(const struct mt_nodedb *db, uint8_t *idx, int max);

/* Display name: long name, else short name, else "!xxxxxxxx". */
const char *mt_node_name(const struct mt_node *n, char *buf, int size);

#ifdef __cplusplus
}
#endif
