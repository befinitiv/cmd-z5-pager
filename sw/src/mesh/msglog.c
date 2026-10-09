#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "msglog.h"

LOG_MODULE_REGISTER(msglog, CONFIG_CMDZ5_APP_LOG_LEVEL);

#define LOG_PATH    "/lfs/msgs.log"
#define NEW_PATH    "/lfs/msgs.new"
#define MAGIC       "MSG1"
/* Rewrite the log when it grows beyond this (40 messages need about 10 KB). */
#ifndef MSGLOG_COMPACT_AT
#define MSGLOG_COMPACT_AT (24 * 1024)
#endif
#define COMPACT_AT MSGLOG_COMPACT_AT

enum rec_type {
	REC_MSG = 1,
	REC_STATUS = 2,
	REC_READ = 3,
};

/* REC_MSG: u16 local id, u32 conv, u32 from, u32 packet id, u32 time,
 * u8 flags (bit 0 outgoing, bit 1 read), u8 status, u8 text length, text.
 */
#define MSG_HEAD 22 /* bytes before the text, including the type */

static bool ok;  /* file system available */
static bool replaying;

static int encode_msg(const struct msg *m, uint8_t *buf)
{
	size_t n = strlen(m->text);

	buf[0] = REC_MSG;
	sys_put_le16(m->local_id, buf + 1);
	sys_put_le32(m->conv, buf + 3);
	sys_put_le32(m->from, buf + 7);
	sys_put_le32(m->packet_id, buf + 11);
	sys_put_le32(m->time, buf + 15);
	buf[19] = (m->outgoing ? 1 : 0) | (m->read ? 2 : 0);
	buf[20] = m->status;
	buf[21] = n;
	memcpy(buf + MSG_HEAD, m->text, n);
	return MSG_HEAD + n;
}

static void append(const uint8_t *rec, size_t len);

/* Write all current messages to a new log and replace the old one. */
static void compact(void)
{
	struct fs_file_t f;
	uint8_t buf[MSG_HEAD + MSG_TEXT_MAX];

	fs_file_t_init(&f);
	if (fs_open(&f, NEW_PATH, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC) != 0) {
		return;
	}
	fs_write(&f, MAGIC, 4);
	for (int i = 0; i < msgstore_count(); i++) {
		fs_write(&f, buf, encode_msg(msgstore_get(i), buf));
	}
	fs_close(&f);
	if (fs_rename(NEW_PATH, LOG_PATH) != 0) {
		LOG_WRN("compaction failed");
	}
}

static void append(const uint8_t *rec, size_t len)
{
	struct fs_file_t f;
	struct fs_dirent st;

	if (!ok || replaying) {
		return;
	}
	bool exists = fs_stat(LOG_PATH, &st) == 0 && st.size > 0;

	if (exists && st.size > COMPACT_AT) {
		/* The store is already up to date, so the new log includes this record. */
		compact();
		return;
	}
	fs_file_t_init(&f);
	if (fs_open(&f, LOG_PATH, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND) != 0) {
		LOG_WRN("cannot open %s", LOG_PATH);
		return;
	}
	/* Not fs_tell(): in append mode it is 0 until the first write. */
	if (!exists) {
		fs_write(&f, MAGIC, 4);
	}
	fs_write(&f, rec, len);
	fs_close(&f);
}

void msglog_add(const struct msg *m)
{
	uint8_t buf[MSG_HEAD + MSG_TEXT_MAX];

	append(buf, encode_msg(m, buf));
}

void msglog_status(uint16_t local_id, uint8_t status)
{
	uint8_t rec[4] = {REC_STATUS};

	sys_put_le16(local_id, rec + 1);
	rec[3] = status;
	append(rec, sizeof(rec));
}

void msglog_read(uint32_t conv)
{
	uint8_t rec[5] = {REC_READ};

	sys_put_le32(conv, rec + 1);
	append(rec, sizeof(rec));
}

static int read_exact(struct fs_file_t *f, void *buf, size_t len)
{
	return fs_read(f, buf, len) == (ssize_t)len ? 0 : -EIO;
}

void msglog_load(void)
{
	struct fs_file_t f;
	struct fs_statvfs vfs;
	uint8_t buf[MSG_HEAD + MSG_TEXT_MAX];
	int records = 0;

	ok = fs_statvfs("/lfs", &vfs) == 0;
	if (!ok) {
		LOG_WRN("no file system: messages are not stored");
		return;
	}
	fs_file_t_init(&f);
	if (fs_open(&f, LOG_PATH, FS_O_READ) != 0) {
		return; /* no messages yet */
	}
	if (read_exact(&f, buf, 4) != 0 || memcmp(buf, MAGIC, 4) != 0) {
		fs_close(&f);
		LOG_WRN("unknown message log format, starting empty");
		fs_unlink(LOG_PATH);
		return;
	}
	replaying = true;
	while (read_exact(&f, buf, 1) == 0) {
		if (buf[0] == REC_MSG) {
			struct msg m = {0};

			if (read_exact(&f, buf + 1, MSG_HEAD - 1) != 0 ||
			    read_exact(&f, m.text, buf[21]) != 0) {
				break; /* truncated last record (power loss): ignore */
			}
			m.local_id = sys_get_le16(buf + 1);
			m.conv = sys_get_le32(buf + 3);
			m.from = sys_get_le32(buf + 7);
			m.packet_id = sys_get_le32(buf + 11);
			m.time = sys_get_le32(buf + 15);
			m.outgoing = buf[19] & 1;
			m.read = buf[19] & 2;
			m.status = buf[20];
			m.text[buf[21]] = '\0';
			msgstore_restore(&m);
		} else if (buf[0] == REC_STATUS) {
			if (read_exact(&f, buf + 1, 3) != 0) {
				break;
			}
			struct msg *m = msgstore_find_local(sys_get_le16(buf + 1));

			if (m) {
				m->status = buf[3];
			}
		} else if (buf[0] == REC_READ) {
			if (read_exact(&f, buf + 1, 4) != 0) {
				break;
			}
			msgstore_mark_read(sys_get_le32(buf + 1));
		} else {
			LOG_WRN("corrupt message log after %d records", records);
			break;
		}
		records++;
	}
	replaying = false;
	fs_close(&f);

	/* Messages that were still on their way when the device stopped. */
	for (int i = 0; i < msgstore_count(); i++) {
		struct msg *m = msgstore_get(i);

		if (m->outgoing && m->status == MSG_QUEUED) {
			m->status = MSG_FAILED;
		}
	}
	LOG_INF("%d messages restored", msgstore_count());
}
