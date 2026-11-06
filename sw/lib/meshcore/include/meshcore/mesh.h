/*
 * MeshCore client ("companion"): what a MeshCore node that does not repeat
 * does with packets. Plain C without threads or allocation, like the
 * Meshtastic router: the owner feeds received frames with mc_mesh_receive(),
 * sends with the mc_mesh_send_*() functions and calls mc_mesh_poll()
 * regularly; frames go out through the transmit callback. Times are
 * milliseconds from a free-running clock.
 *
 * Behaviour follows MeshCore's BaseChatMesh and companion firmware:
 * - direct messages go along the contact's known path, else by flood; the
 *   receiver acknowledges with an ACK, or after a flood with a PATH packet
 *   that carries the ACK and the path back, which becomes our route to it;
 *   a message is sent up to MC_ATTEMPTS times, the last time by flood;
 * - group (channel) messages are flooded; hearing a repeater send ours on
 *   counts as delivered;
 * - floods carry the region scope's transport code when one is set
 *   (repeaters with regions only forward scoped packets they know).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "meshcore/packet.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MC_MAX_TEXT     160 /* MeshCore's MAX_TEXT_LEN */
#define MC_CHANNELS     4
#define MC_TXQ_LEN      8
#define MC_PENDING_LEN  4
#define MC_SEEN_LEN     64
#define MC_ATTEMPTS     3
#define MC_MAX_PEERS    4   /* contacts sharing a hash byte that are tried */

enum mc_tx_status {
	MC_TX_SENT,   /* transmitted (first time) */
	MC_TX_ACKED,  /* direct message acknowledged, or channel message repeated */
	MC_TX_NO_ACK, /* channel message: no repeater was heard sending it on */
	MC_TX_FAILED, /* direct message: no ACK after all attempts */
};

/* A contact, as far as sending to it or decrypting its packets needs. */
struct mc_contact {
	uint32_t num;      /* the application's id */
	uint8_t pub[32];
	uint8_t secret[32]; /* mc_shared_secret() */
	uint8_t path_len;   /* MC_PATH_UNKNOWN: flood */
	uint8_t path[MC_MAX_PATH];
};

/* How a packet arrived. */
struct mc_rx_info {
	int16_t rssi;
	int8_t snr;
	bool flood;
	uint8_t hops; /* repeaters on the way (floods, 0 for zero-hop adverts), 0xff unknown */
	uint8_t path_len;
	const uint8_t *path;
};

struct mc_mesh_ops {
	void (*transmit)(void *ctx, const uint8_t *frame, size_t len);
	uint32_t (*random)(void *ctx);
	/* Contacts whose public key starts with the hash byte; returns how many (<= max). */
	int (*find_peers)(void *ctx, uint8_t hash, struct mc_contact *peers, int max);
	/* A valid advert (not our own). */
	void (*advert)(void *ctx, const struct mc_advert *a, const struct mc_rx_info *rx);
	/* A direct message (also a room server's post) from a contact. */
	void (*text)(void *ctx, uint32_t from, uint32_t timestamp, const char *text,
		     const struct mc_rx_info *rx);
	/* A channel message: text is "sender: message". */
	void (*group_text)(void *ctx, int channel, uint32_t timestamp, const char *text,
			   const struct mc_rx_info *rx);
	/* A route to a contact (MC_PATH_UNKNOWN: forget it, flood next time). */
	void (*path)(void *ctx, uint32_t num, const uint8_t *path, uint8_t path_len);
	void (*status)(void *ctx, uint32_t id, enum mc_tx_status status);
};

struct mc_txq_entry {
	bool used;
	uint32_t due;
	uint32_t id; /* message whose first transmission this is, or 0 */
	uint8_t len;
	uint8_t frame[MC_MAX_FRAME];
};

struct mc_pending {
	bool used;
	bool group;
	uint32_t id;
	uint32_t deadline;
	/* Group message: its packet hash, to recognise it repeated. */
	uint8_t hash[MC_HASH_SIZE];
	/* Direct message. */
	struct mc_contact to;
	uint32_t timestamp;
	uint8_t attempt;
	uint32_t acks[MC_ATTEMPTS]; /* expected ACK of each attempt */
	char text[MC_MAX_TEXT + 1];
};

struct mc_channel {
	bool used;
	uint8_t secret[32]; /* 16 bytes, zero-padded for the MAC */
	uint8_t hash;
};

struct mc_mesh {
	const struct mc_mesh_ops *ops;
	void *ctx;
	uint8_t pub[32], prv[64];

	/* Radio settings, for airtime estimates. */
	uint8_t sf, cr;
	uint32_t bw_hz;
	uint16_t preamble;

	bool scoped;
	uint8_t scope[16];
	struct mc_channel channels[MC_CHANNELS];

	uint8_t seen[MC_SEEN_LEN][MC_HASH_SIZE];
	int seen_pos;
	/* Recently delivered direct messages (sender, time): retries are not shown twice. */
	struct {
		uint32_t from, timestamp;
	} recent[8];
	int recent_pos;

	struct mc_txq_entry txq[MC_TXQ_LEN];
	struct mc_pending pending[MC_PENDING_LEN];
	uint32_t now;

	/* Statistics */
	uint32_t rx_packets, rx_dupes, rx_undecoded, tx_packets;
};

void mc_mesh_init(struct mc_mesh *m, const struct mc_mesh_ops *ops, void *ctx,
		  const uint8_t pub[32], const uint8_t prv[64]);
void mc_mesh_set_radio(struct mc_mesh *m, uint8_t sf, uint32_t bw_hz, uint8_t cr,
		       uint16_t preamble);
/* Region scope for floods; NULL: unscoped. */
void mc_mesh_set_scope(struct mc_mesh *m, const uint8_t key[16]);
/* Channel slot i (0..MC_CHANNELS-1); secret NULL clears it. */
void mc_mesh_set_channel(struct mc_mesh *m, int i, const uint8_t secret[16]);

/* Our advert, signed: zero-hop (direct neighbours) or flooded through the mesh. */
void mc_mesh_send_advert(struct mc_mesh *m, const struct mc_advert *a, bool flood);
/* Returns the message id, or 0 if the queue is full or the text too long. */
uint32_t mc_mesh_send_text(struct mc_mesh *m, const struct mc_contact *to, uint32_t timestamp,
			   const char *text);
/* "sender: text" on a channel; the text is cut to fit. Returns the id, or 0. */
uint32_t mc_mesh_send_group(struct mc_mesh *m, int channel, uint32_t timestamp,
			    const char *sender, const char *text);

void mc_mesh_receive(struct mc_mesh *m, const uint8_t *frame, size_t len, int16_t rssi,
		     int8_t snr, uint32_t now);

/* Transmit what is due, handle timeouts; returns the ms until the next event. */
uint32_t mc_mesh_poll(struct mc_mesh *m, uint32_t now);

/* Time on air of a frame of len bytes (ms). */
uint32_t mc_mesh_airtime_ms(const struct mc_mesh *m, size_t len);

#ifdef __cplusplus
}
#endif
