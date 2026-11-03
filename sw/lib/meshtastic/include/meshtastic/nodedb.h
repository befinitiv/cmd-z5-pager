/*
 * Known nodes: names, public keys, position and radio statistics. Fixed size;
 * when the table is full a node is replaced: one without user info if there
 * is one, else the least recently heard (by last_heard, then heard_unix).
 * Nodes the application keeps (keep()) are replaced only if all are kept.
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
	/* Wall-clock time last heard, 0 if unknown; kept by the application across restarts. */
	uint32_t heard_unix;
	int16_t rssi;
	int8_t snr;
	uint8_t hops_away;   /* 0 = direct */
	/* Latest telemetry values (device and environment merged), not stored. */
	struct mt_telemetry telemetry;
	/* Cached PKI key (SHA-256 of the X25519 secret), see the router. */
	bool has_shared_key;
	uint8_t shared_key[32];
};

struct mt_nodedb {
	struct mt_node nodes[MT_MAX_NODES];
	int count;
	/* Optional: nodes not to replace while others can be; ctx is passed. */
	bool (*keep)(void *ctx, const struct mt_node *n);
	/* Optional: a node is about to be replaced. */
	void (*evicted)(void *ctx, const struct mt_node *n);
	void *ctx;
};

void mt_nodedb_init(struct mt_nodedb *db);
struct mt_node *mt_nodedb_find(struct mt_nodedb *db, uint32_t num);
/* Find or create (replacing a node when full, see above). */
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
