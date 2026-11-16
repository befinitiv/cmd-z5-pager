#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "msglog.h"

LOG_MODULE_REGISTER(msglog, CONFIG_CMDZ5_APP_LOG_LEVEL);

/* Meshtastic's messages, and MeshCore's in their own log (msglog_select()). */
static const char *log_path = "/lfs/msgs.log";
static const char *new_path = "/lfs/msgs.new";
#define LOG_PATH log_path
#define NEW_PATH new_path
#define MAGIC       "MSG1"
/* Rewrite the log when it grows beyond this (64 messages need at most 19 KB). */
#ifndef MSGLOG_COMPACT_AT
#define MSGLOG_COMPACT_AT (24 * 1024)
#endif
#define COMPACT_AT MSGLOG_COMPACT_AT

enum rec_type {
	REC_MSG = 1,
	REC_STATUS = 2,
	REC_READ = 3,
	REC_DELETE = 4, /* u32 conv: all its messages deleted */
	REC_MSG2 = 5,
	REC_TIME = 6, /* u16 local id, u32 time: set when the clock became valid */
	REC_MSG3 = 7,
	REC_PATH = 8, /* u16 local id, u8 hops, u8 bytes per hop, the path */
};

/*
 * REC_MSG: u16 local id, u32 conv, u32 from, u32 packet id, u32 time,
 * u8 flags (bit 0 outgoing, bit 1 read), u8 status, u8 text length, text.
 * REC_MSG2 (written now): the same, flags bit 2 radio data, bit 3 hops of
 * our route to the sender (MeshCore, see struct msg), and before the
 * text i16 RSSI, i8 SNR, u8 hops.
 * REC_MSG3 (written now): REC_MSG2, and before the text u8 path hops,
 * u8 bytes per hop and the path (see struct msg).
 */
#define MSG_HEAD  22 /* bytes before the text (and path), including the type */
#define MSG2_HEAD 26
#define MSG3_HEAD 28
#define REC_MAX   (MSG3_HEAD + MSG_PATH_MAX + MSG_TEXT_MAX)

static bool ok;  /* file system available */

void msglog_select(bool meshcore)
{
	log_path = meshcore ? "/lfs/mcmsgs.log" : "/lfs/msgs.log";
	new_path = meshcore ? "/lfs/mcmsgs.new" : "/lfs/msgs.new";
}
static bool replaying;

static int encode_msg(const struct msg *m, uint8_t *buf)
{
	size_t n = strlen(m->text), p = m->path_hops * m->path_size;

	buf[0] = REC_MSG3;
	sys_put_le16(m->local_id, buf + 1);
	sys_put_le32(m->conv, buf + 3);
	sys_put_le32(m->from, buf + 7);
	sys_put_le32(m->packet_id, buf + 11);
	sys_put_le32(m->time, buf + 15);
	buf[19] = (m->outgoing ? 1 : 0) | (m->read ? 2 : 0) | (m->has_radio ? 4 : 0) |
		 (m->route_hops ? 8 : 0);
	buf[20] = m->status;
	buf[21] = n;
	sys_put_le16(m->rssi, buf + 22);
	buf[24] = m->snr;
	buf[25] = m->hops;
	buf[26] = m->path_hops;
	buf[27] = m->path_size;
	memcpy(buf + MSG3_HEAD, m->path, p);
	memcpy(buf + MSG3_HEAD + p, m->text, n);
	return MSG3_HEAD + p + n;
}

static void append(const uint8_t *rec, size_t len);

/* Write all current messages to a new log and replace the old one. */
static void compact(void)
{
	struct fs_file_t f;
	uint8_t buf[REC_MAX];

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
	uint8_t buf[REC_MAX];

	append(buf, encode_msg(m, buf));
}

void msglog_status(uint16_t local_id, uint8_t status)
{
	uint8_t rec[4] = {REC_STATUS};

	sys_put_le16(local_id, rec + 1);
	rec[3] = status;
	append(rec, sizeof(rec));
}

void msglog_time(uint16_t local_id, uint32_t time)
{
	uint8_t rec[7] = {REC_TIME};

	sys_put_le16(local_id, rec + 1);
	sys_put_le32(time, rec + 3);
	append(rec, sizeof(rec));
}

void msglog_path(const struct msg *m)
{
	uint8_t rec[5 + MSG_PATH_MAX] = {REC_PATH};
	size_t p = m->path_hops * m->path_size;

	sys_put_le16(m->local_id, rec + 1);
	rec[3] = m->path_hops;
	rec[4] = m->path_size;
	memcpy(rec + 5, m->path, p);
	append(rec, 5 + p);
}

/* A path from the log: hops and bytes per hop, if they fit. */
static bool path_ok(uint8_t hops, uint8_t size)
{
	return hops * size <= MSG_PATH_MAX;
}

void msglog_read(uint32_t conv)
{
	uint8_t rec[5] = {REC_READ};

	sys_put_le32(conv, rec + 1);
	append(rec, sizeof(rec));
}

void msglog_delete(uint32_t conv)
{
	uint8_t rec[5] = {REC_DELETE};

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
	uint8_t buf[REC_MAX];
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
		if (buf[0] == REC_MSG || buf[0] == REC_MSG2 || buf[0] == REC_MSG3) {
			struct msg m = {0};
			size_t head = buf[0] == REC_MSG3 ? MSG3_HEAD
				      : buf[0] == REC_MSG2 ? MSG2_HEAD : MSG_HEAD;

			if (read_exact(&f, buf + 1, head - 1) != 0) {
				break; /* truncated last record (power loss): ignore */
			}
			if (buf[0] == REC_MSG3) {
				if (!path_ok(buf[26], buf[27])) {
					LOG_WRN("corrupt message log after %d records", records);
					break;
				}
				m.path_hops = buf[26];
				m.path_size = buf[27];
				if (read_exact(&f, m.path, m.path_hops * m.path_size) != 0) {
					break;
				}
			}
			if (read_exact(&f, m.text, buf[21]) != 0) {
				break;
			}
			if (buf[0] != REC_MSG) {
				m.has_radio = buf[19] & 4;
				m.route_hops = buf[19] & 8;
				m.rssi = (int16_t)sys_get_le16(buf + 22);
				m.snr = (int8_t)buf[24];
				m.hops = buf[25];
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
		} else if (buf[0] == REC_PATH) {
			uint8_t path[MSG_PATH_MAX];

			if (read_exact(&f, buf + 1, 4) != 0) {
				break;
			}
			if (!path_ok(buf[3], buf[4])) {
				LOG_WRN("corrupt message log after %d records", records);
				break;
			}
			if (read_exact(&f, path, buf[3] * buf[4]) != 0) {
				break;
			}
			struct msg *m = msgstore_find_local(sys_get_le16(buf + 1));

			if (m) {
				m->path_hops = buf[3];
				m->path_size = buf[4];
				memcpy(m->path, path, buf[3] * buf[4]);
			}
		} else if (buf[0] == REC_TIME) {
			if (read_exact(&f, buf + 1, 6) != 0) {
				break;
			}
			struct msg *m = msgstore_find_local(sys_get_le16(buf + 1));

			if (m) {
				m->time = sys_get_le32(buf + 3);
			}
		} else if (buf[0] == REC_READ) {
			if (read_exact(&f, buf + 1, 4) != 0) {
				break;
			}
			msgstore_mark_read(sys_get_le32(buf + 1));
		} else if (buf[0] == REC_DELETE) {
			if (read_exact(&f, buf + 1, 4) != 0) {
				break;
			}
			msgstore_delete_conv(sys_get_le32(buf + 1));
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
