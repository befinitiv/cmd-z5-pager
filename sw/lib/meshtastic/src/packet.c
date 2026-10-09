/*
 * Header codec and a minimal protobuf reader/writer for the messages used.
 */

#include <string.h>

#include "meshtastic/packet.h"

#define FLAG_HOP_LIMIT 0x07
#define FLAG_WANT_ACK  0x08
#define FLAG_VIA_MQTT  0x10
#define HOP_START_SHIFT 5

#define WT_VARINT  0
#define WT_FIXED64 1
#define WT_LEN     2
#define WT_FIXED32 5

/* Data.bitfield bits */
#define BITFIELD_WANT_RESPONSE (1u << 1)

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

static uint32_t get32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

void mt_header_encode(const struct mt_header *h, uint8_t out[MT_HEADER_LEN])
{
	put32(out, h->to);
	put32(out + 4, h->from);
	put32(out + 8, h->id);
	out[12] = (h->hop_limit & FLAG_HOP_LIMIT) | (h->want_ack ? FLAG_WANT_ACK : 0) |
		  (h->via_mqtt ? FLAG_VIA_MQTT : 0) | (h->hop_start << HOP_START_SHIFT);
	out[13] = h->channel;
	out[14] = h->next_hop;
	out[15] = h->relay_node;
}

void mt_header_decode(const uint8_t in[MT_HEADER_LEN], struct mt_header *h)
{
	h->to = get32(in);
	h->from = get32(in + 4);
	h->id = get32(in + 8);
	h->hop_limit = in[12] & FLAG_HOP_LIMIT;
	h->want_ack = in[12] & FLAG_WANT_ACK;
	h->via_mqtt = in[12] & FLAG_VIA_MQTT;
	h->hop_start = in[12] >> HOP_START_SHIFT;
	h->channel = in[13];
	h->next_hop = in[14];
	h->relay_node = in[15];
}

/* Writer ------------------------------------------------------------------- */

struct pbw {
	uint8_t *buf;
	size_t size, len;
	bool err;
};

static void w_byte(struct pbw *w, uint8_t b)
{
	if (w->len < w->size) {
		w->buf[w->len++] = b;
	} else {
		w->err = true;
	}
}

static void w_raw_varint(struct pbw *w, uint64_t v)
{
	do {
		w_byte(w, (v & 0x7f) | (v > 0x7f ? 0x80 : 0));
		v >>= 7;
	} while (v);
}

static void w_tag(struct pbw *w, uint32_t field, uint32_t wt)
{
	w_raw_varint(w, field << 3 | wt);
}

static void w_varint(struct pbw *w, uint32_t field, uint64_t v)
{
	w_tag(w, field, WT_VARINT);
	w_raw_varint(w, v);
}

static void w_fixed32(struct pbw *w, uint32_t field, uint32_t v)
{
	w_tag(w, field, WT_FIXED32);
	for (int i = 0; i < 4; i++) {
		w_byte(w, v >> (8 * i));
	}
}

static void w_bytes(struct pbw *w, uint32_t field, const void *p, size_t n)
{
	w_tag(w, field, WT_LEN);
	w_raw_varint(w, n);
	for (size_t i = 0; i < n; i++) {
		w_byte(w, ((const uint8_t *)p)[i]);
	}
}

static void w_string(struct pbw *w, uint32_t field, const char *s)
{
	if (*s) {
		w_bytes(w, field, s, strlen(s));
	}
}

static int w_result(const struct pbw *w)
{
	return w->err ? -1 : (int)w->len;
}

/* Reader ------------------------------------------------------------------- */

struct pbr {
	const uint8_t *p, *end;
	bool err;
};

struct pb_field {
	uint32_t num;
	uint32_t wt;
	uint64_t v;         /* varint and fixed values */
	const uint8_t *data; /* length-delimited */
	size_t len;
};

static bool r_varint(struct pbr *r, uint64_t *v)
{
	*v = 0;
	for (int shift = 0; shift < 64; shift += 7) {
		if (r->p >= r->end) {
			return false;
		}
		uint8_t b = *r->p++;

		*v |= (uint64_t)(b & 0x7f) << shift;
		if (!(b & 0x80)) {
			return true;
		}
	}
	return false;
}

/* Next field; false at the end or on malformed data (r->err set). */
static bool r_next(struct pbr *r, struct pb_field *f)
{
	uint64_t tag;

	if (r->p >= r->end) {
		return false;
	}
	if (!r_varint(r, &tag) || (tag >> 3) == 0) {
		r->err = true;
		return false;
	}
	f->num = tag >> 3;
	f->wt = tag & 7;
	switch (f->wt) {
	case WT_VARINT:
		if (!r_varint(r, &f->v)) {
			r->err = true;
			return false;
		}
		return true;
	case WT_FIXED32:
	case WT_FIXED64: {
		size_t n = f->wt == WT_FIXED32 ? 4 : 8;

		if ((size_t)(r->end - r->p) < n) {
			r->err = true;
			return false;
		}
		f->v = 0;
		for (size_t i = 0; i < n; i++) {
			f->v |= (uint64_t)r->p[i] << (8 * i);
		}
		r->p += n;
		return true;
	}
	case WT_LEN: {
		uint64_t n;

		if (!r_varint(r, &n) || n > (uint64_t)(r->end - r->p)) {
			r->err = true;
			return false;
		}
		f->data = r->p;
		f->len = n;
		r->p += n;
		return true;
	}
	default:
		r->err = true;
		return false;
	}
}

static void copy_string(char *dst, size_t size, const struct pb_field *f)
{
	size_t n = f->len < size - 1 ? f->len : size - 1;

	memcpy(dst, f->data, n);
	dst[n] = '\0';
}

/* Messages ----------------------------------------------------------------- */

int mt_data_encode(const struct mt_data *d, uint8_t *buf, size_t size)
{
	struct pbw w = {.buf = buf, .size = size};

	w_varint(&w, 1, d->portnum);
	w_bytes(&w, 2, d->payload, d->payload_len);
	if (d->want_response) {
		w_varint(&w, 3, 1);
	}
	if (d->dest) {
		w_fixed32(&w, 4, d->dest);
	}
	if (d->source) {
		w_fixed32(&w, 5, d->source);
	}
	if (d->request_id) {
		w_fixed32(&w, 6, d->request_id);
	}
	if (d->reply_id) {
		w_fixed32(&w, 7, d->reply_id);
	}
	if (d->emoji) {
		w_fixed32(&w, 8, d->emoji);
	}
	/* Like the firmware: bitfield is always present (ok_to_mqtt off). */
	w_varint(&w, 9, d->want_response ? BITFIELD_WANT_RESPONSE : 0);
	return w_result(&w);
}

int mt_data_decode(const uint8_t *buf, size_t len, struct mt_data *d)
{
	struct pbr r = {.p = buf, .end = buf + len};
	struct pb_field f;

	memset(d, 0, sizeof(*d));
	d->payload = buf;
	while (r_next(&r, &f)) {
		switch (f.num) {
		case 1:
			d->portnum = f.v;
			break;
		case 2:
			if (f.wt != WT_LEN) {
				return -1;
			}
			d->payload = f.data;
			d->payload_len = f.len;
			break;
		case 3:
			d->want_response = f.v != 0;
			break;
		case 4:
			d->dest = f.v;
			break;
		case 5:
			d->source = f.v;
			break;
		case 6:
			d->request_id = f.v;
			break;
		case 7:
			d->reply_id = f.v;
			break;
		case 8:
			d->emoji = f.v;
			break;
		case 9:
			if (f.v & BITFIELD_WANT_RESPONSE) {
				d->want_response = true;
			}
			break;
		default:
			break;
		}
	}
	/* A wrong key yields garbage: reject anything without a valid port. */
	return r.err || d->portnum == 0 ? -1 : 0;
}

int mt_user_encode(const struct mt_user *u, uint8_t *buf, size_t size)
{
	struct pbw w = {.buf = buf, .size = size};

	w_string(&w, 1, u->id);
	w_string(&w, 2, u->long_name);
	w_string(&w, 3, u->short_name);
	if (u->hw_model) {
		w_varint(&w, 5, u->hw_model);
	}
	if (u->role) {
		w_varint(&w, 7, u->role);
	}
	if (u->has_public_key) {
		w_bytes(&w, 8, u->public_key, 32);
	}
	return w_result(&w);
}

int mt_user_decode(const uint8_t *buf, size_t len, struct mt_user *u)
{
	struct pbr r = {.p = buf, .end = buf + len};
	struct pb_field f;

	memset(u, 0, sizeof(*u));
	while (r_next(&r, &f)) {
		switch (f.num) {
		case 1:
			copy_string(u->id, sizeof(u->id), &f);
			break;
		case 2:
			copy_string(u->long_name, sizeof(u->long_name), &f);
			break;
		case 3:
			copy_string(u->short_name, sizeof(u->short_name), &f);
			break;
		case 5:
			u->hw_model = f.v;
			break;
		case 7:
			u->role = f.v;
			break;
		case 8:
			if (f.wt == WT_LEN && f.len == 32) {
				memcpy(u->public_key, f.data, 32);
				u->has_public_key = true;
			}
			break;
		default:
			break;
		}
	}
	return r.err ? -1 : 0;
}

int mt_position_encode(const struct mt_position *p, uint8_t *buf, size_t size)
{
	struct pbw w = {.buf = buf, .size = size};

	if (p->has_latlon) {
		w_fixed32(&w, 1, (uint32_t)p->latitude_i);
		w_fixed32(&w, 2, (uint32_t)p->longitude_i);
	}
	if (p->has_altitude) {
		w_varint(&w, 3, (uint64_t)(int64_t)p->altitude); /* int32: sign-extended */
	}
	if (p->time) {
		w_fixed32(&w, 4, p->time);
	}
	if (p->precision_bits) {
		w_varint(&w, 23, p->precision_bits);
	}
	return w_result(&w);
}

int mt_position_decode(const uint8_t *buf, size_t len, struct mt_position *p)
{
	struct pbr r = {.p = buf, .end = buf + len};
	struct pb_field f;
	bool lat = false, lon = false;

	memset(p, 0, sizeof(*p));
	while (r_next(&r, &f)) {
		switch (f.num) {
		case 1:
			p->latitude_i = (int32_t)f.v;
			lat = true;
			break;
		case 2:
			p->longitude_i = (int32_t)f.v;
			lon = true;
			break;
		case 3:
			p->altitude = (int32_t)f.v;
			p->has_altitude = true;
			break;
		case 4:
			p->time = f.v;
			break;
		case 23:
			p->precision_bits = f.v;
			break;
		default:
			break;
		}
	}
	p->has_latlon = lat && lon;
	return r.err ? -1 : 0;
}

int mt_routing_encode(uint32_t error_reason, uint8_t *buf, size_t size)
{
	struct pbw w = {.buf = buf, .size = size};

	/* oneof member: written even when it is 0 */
	w_varint(&w, 3, error_reason);
	return w_result(&w);
}

int mt_routing_decode(const uint8_t *buf, size_t len, uint32_t *error_reason)
{
	struct pbr r = {.p = buf, .end = buf + len};
	struct pb_field f;

	*error_reason = 0;
	while (r_next(&r, &f)) {
		if (f.num == 3) {
			*error_reason = f.v;
		}
	}
	return r.err ? -1 : 0;
}
