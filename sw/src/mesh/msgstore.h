/*
 * Text messages of all conversations (the primary channel and one per node
 * for direct messages), oldest replaced first. Kept in RAM for now; storage in
 * LittleFS follows with the power phase.
 *
 * Shared between the mesh thread and the UI: lock around any access.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MSG_MAX       40
#define MSG_TEXT_MAX  200
/* Conversation id of the primary channel; direct messages use the node number. */
#define CONV_CHANNEL  0xffffffffu

enum msg_status {
	MSG_RECEIVED,
	MSG_QUEUED,
	MSG_SENT,
	MSG_DELIVERED, /* ACK from the destination (or a relay, for the channel) */
	MSG_NO_ACK,    /* channel message nobody relayed */
	MSG_FAILED,
};

struct msg {
	uint32_t conv;
	uint32_t from;
	uint32_t packet_id;
	uint16_t local_id;
	uint32_t time; /* unix time, or 0 if the clock was not set */
	bool outgoing;
	bool read;
	uint8_t status;
	char text[MSG_TEXT_MAX + 1];
};

void msgstore_lock(void);
void msgstore_unlock(void);

/* Add a message (copied); returns its local id. */
uint16_t msgstore_add(const struct msg *m);
int msgstore_count(void);
/* i = 0 is the oldest message. */
struct msg *msgstore_get(int i);
struct msg *msgstore_find_local(uint16_t local_id);
/* Update the status of an outgoing message by packet id; false if unknown. */
bool msgstore_set_status(uint32_t packet_id, enum msg_status status);

int msgstore_unread(uint32_t conv); /* conv = 0: all conversations */
void msgstore_mark_read(uint32_t conv);
/* Conversations, the channel first, then by the newest message. */
int msgstore_conversations(uint32_t *convs, int max);
/* Newest message of a conversation, or NULL. */
struct msg *msgstore_last(uint32_t conv);
