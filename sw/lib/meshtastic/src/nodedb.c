#include <stdio.h>
#include <string.h>

#include "meshtastic/nodedb.h"

void mt_nodedb_init(struct mt_nodedb *db)
{
	memset(db, 0, sizeof(*db));
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
		n = &db->nodes[0];
		for (int i = 1; i < db->count; i++) {
			if (db->nodes[i].last_heard < n->last_heard) {
				n = &db->nodes[i];
			}
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
