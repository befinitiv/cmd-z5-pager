#include <stdio.h>
#include <string.h>

#include "meshtastic/nodedb.h"

void mt_nodedb_init(struct mt_nodedb *db)
{
	memset(db->nodes, 0, sizeof(db->nodes));
	db->count = 0;
}

/* True if a should be replaced before b. */
static bool replace_first(const struct mt_node *a, const struct mt_node *b)
{
	if (a->has_user != b->has_user) {
		return !a->has_user;
	}
	if (a->last_heard != b->last_heard) {
		return a->last_heard < b->last_heard;
	}
	return a->heard_unix < b->heard_unix;
}

static struct mt_node *victim(struct mt_nodedb *db)
{
	struct mt_node *n = NULL;

	/* First among the nodes not kept, then among all. */
	for (int pass = 0; pass < 2 && !n; pass++) {
		for (int i = 0; i < db->count; i++) {
			struct mt_node *c = &db->nodes[i];

			if (pass == 0 && db->keep && db->keep(db->ctx, c)) {
				continue;
			}
			if (!n || replace_first(c, n)) {
				n = c;
			}
		}
	}
	return n;
}

struct mt_node *mt_nodedb_find(struct mt_nodedb *db, uint32_t num)
{
	for (int i = 0; i < db->count; i++) {
		if (db->nodes[i].num == num) {
			return &db->nodes[i];
		}
	}
	return NULL;
}

struct mt_node *mt_nodedb_get(struct mt_nodedb *db, uint32_t num, uint32_t now)
{
	struct mt_node *n = mt_nodedb_find(db, num);

	if (n) {
		return n;
	}
	if (db->count < MT_MAX_NODES) {
		n = &db->nodes[db->count++];
	} else {
		n = victim(db);
		if (db->evicted) {
			db->evicted(db->ctx, n);
		}
	}
	memset(n, 0, sizeof(*n));
	n->num = num;
	n->last_heard = now;
	return n;
}

void mt_nodedb_set_user(struct mt_node *n, const struct mt_user *u)
{
	if (!n->has_user || u->has_public_key != n->user.has_public_key ||
	    memcmp(u->public_key, n->user.public_key, 32) != 0) {
		n->has_shared_key = false;
	}
	n->user = *u;
	n->has_user = true;
}

int mt_nodedb_sorted(const struct mt_nodedb *db, uint8_t *idx, int max)
{
	int n = 0;

	for (int i = 0; i < db->count && n < max; i++) {
		int j = n++;

		while (j > 0 && db->nodes[idx[j - 1]].last_heard < db->nodes[i].last_heard) {
			idx[j] = idx[j - 1];
			j--;
		}
		idx[j] = i;
	}
	return n;
}

const char *mt_node_name(const struct mt_node *n, char *buf, int size)
{
	if (n && n->has_user && n->user.long_name[0]) {
		return n->user.long_name;
	}
	if (n && n->has_user && n->user.short_name[0]) {
		return n->user.short_name;
	}
	snprintf(buf, size, "!%08x", n ? (unsigned)n->num : 0u);
	return buf;
}
