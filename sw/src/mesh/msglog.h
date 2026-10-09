/*
 * Persistent message log in LittleFS (/lfs/msgs.log): new messages, status
 * changes and "read" marks are appended as small records and replayed at
 * startup. When the log grows too large it is rewritten with the current
 * messages only (compaction).
 *
 * Called by the message store with its lock held.
 */

#pragma once

#include <stdint.h>

#include "msgstore.h"

/* Replay the log into the message store (at startup). */
void msglog_load(void);

void msglog_add(const struct msg *m);
void msglog_status(uint16_t local_id, uint8_t status);
void msglog_read(uint32_t conv);
