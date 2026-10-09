#include <string.h>

#include <zephyr/kernel.h>

#include "msglog.h"
#include "msgstore.h"

static K_MUTEX_DEFINE(lock);
static struct msg msgs[MSG_MAX];
static int head, count;
static uint16_t next_local = 1;

void msgstore_lock(void)
{
	k_mutex_lock(&lock, K_FOREVER);
}

void msgstore_unlock(void)
{
	k_mutex_unlock(&lock);
}

static struct msg *new_slot(void)
{
	int slot;

	if (count < MSG_MAX) {
		slot = (head + count++) % MSG_MAX;
	} else {
		slot = head;
		head = (head + 1) % MSG_MAX;
	}
	return &msgs[slot];
}

uint16_t msgstore_add(const struct msg *m)
{
	struct msg *s = new_slot();

	*s = *m;
	s->local_id = next_local++;
	if (next_local == 0) {
		next_local = 1;
	}
	msglog_add(s);
	return s->local_id;
}

void msgstore_restore(const struct msg *m)
{
	*new_slot() = *m;
	/* New messages continue after the highest stored id. */
	if (m->local_id >= next_local) {
		next_local = m->local_id + 1;
	}
	if (next_local == 0) {
		next_local = 1;
	}
}

void msgstore_init(void)
{
	msgstore_lock();
	msglog_load();
	msgstore_unlock();
}

int msgstore_count(void)
{
	return count;
}

struct msg *msgstore_get(int i)
{
	return i >= 0 && i < count ? &msgs[(head + i) % MSG_MAX] : NULL;
}

struct msg *msgstore_find_local(uint16_t local_id)
{
	for (int i = 0; i < count; i++) {
		struct msg *m = msgstore_get(i);

		if (m->local_id == local_id) {
			return m;
		}
	}
	return NULL;
}

bool msgstore_set_status(uint32_t packet_id, enum msg_status status)
{
	for (int i = 0; i < count; i++) {
		struct msg *m = msgstore_get(i);

		if (m->outgoing && m->packet_id == packet_id) {
			/* Never downgrade a delivered message (a late SENT after an ACK). */
			if (!(m->status == MSG_DELIVERED && status == MSG_SENT) &&
			    m->status != status) {
				m->status = status;
				msglog_status(m->local_id, status);
			}
			return true;
		}
	}
	return false;
}

int msgstore_unread(uint32_t conv)
{
	int n = 0;

	for (int i = 0; i < count; i++) {
		struct msg *m = msgstore_get(i);

		n += !m->read && !m->outgoing && (conv == 0 || m->conv == conv);
	}
	return n;
}

void msgstore_set_sent(uint16_t local_id, uint32_t packet_id, enum msg_status status)
{
	struct msg *m = msgstore_find_local(local_id);

	if (m) {
		m->packet_id = packet_id;
		m->status = status;
		msglog_status(local_id, status);
	}
}

void msgstore_mark_read(uint32_t conv)
{
	bool changed = false;

	for (int i = 0; i < count; i++) {
		struct msg *m = msgstore_get(i);

		if (m->conv == conv && !m->read) {
			m->read = true;
			changed = true;
		}
	}
	if (changed) {
		msglog_read(conv);
	}
}

struct msg *msgstore_newest_unread(void)
{
	for (int i = count - 1; i >= 0; i--) {
		struct msg *m = msgstore_get(i);

		if (!m->read && !m->outgoing) {
			return m;
		}
	}
	return NULL;
}

struct msg *msgstore_last(uint32_t conv)
{
	for (int i = count - 1; i >= 0; i--) {
		struct msg *m = msgstore_get(i);

		if (m->conv == conv) {
			return m;
		}
	}
	return NULL;
}

int msgstore_conversations(uint32_t *convs, int max)
{
	int n = 0;

	if (max > 0) {
		convs[n++] = CONV_CHANNEL;
	}
	/* Newest first: walk backwards, skip conversations already listed. */
	for (int i = count - 1; i >= 0 && n < max; i--) {
		uint32_t c = msgstore_get(i)->conv;
		bool dup = false;

		for (int j = 0; j < n; j++) {
			dup |= convs[j] == c;
		}
		if (!dup) {
			convs[n++] = c;
		}
	}
	return n;
}
