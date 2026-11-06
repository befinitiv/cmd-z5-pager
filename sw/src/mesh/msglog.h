/*
 * Persistent message log in LittleFS (/lfs/msgs.log, MeshCore's in
 * /lfs/mcmsgs.log): new messages, status
 * changes and "read" marks are appended as small records and replayed at
 * startup. When the log grows too large it is rewritten with the current
 * messages only (compaction).
 *
 * Called by the message store with its lock held.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "msgstore.h"

/* The log of MeshCore's messages instead of Meshtastic's (before msglog_load()). */
void msglog_select(bool meshcore);
/* Replay the log into the message store (at startup). */
void msglog_load(void);

void msglog_add(const struct msg *m);
void msglog_status(uint16_t local_id, uint8_t status);
void msglog_time(uint16_t local_id, uint32_t time);
void msglog_read(uint32_t conv);
void msglog_delete(uint32_t conv);
