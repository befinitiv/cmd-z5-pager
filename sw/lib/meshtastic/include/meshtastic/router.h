/*
 * Meshtastic router: encryption, managed flooding, acknowledgements.
 *
 * Plain C without threads or allocation. The owner feeds received frames with
 * mt_router_receive(), sends with mt_router_send() and calls mt_router_poll()
 * regularly; frames go out through the transmit callback. Times are
 * milliseconds from a free-running clock.
 *
 * Behaviour follows the Meshtastic firmware (FloodingRouter, ReliableRouter):
 * - every new packet not addressed to us is rebroadcast once with hop_limit - 1
 *   after a delay that is longer for a better SNR (distant nodes relay first);
 *   hearing another node relay it first cancels our rebroadcast;
 * - packets with want_ack addressed to us are acknowledged with a Routing
 *   message; our own want_ack packets are retransmitted up to 3 times until an
 *   ACK arrives (broadcasts: until we hear a rebroadcast, the implicit ACK);
 * - direct text messages are encrypted with PKI (X25519 + AES-CCM) when the
 *   destination's public key is known, everything else with the channel key.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "meshtastic/channel.h"
#include "meshtastic/crypto.h"
#include "meshtastic/nodedb.h"
#include "meshtastic/packet.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MT_MAX_CHANNELS 2
#define MT_TXQ_LEN      8
#define MT_PENDING_LEN  4
#define MT_SEEN_LEN     64
#define MT_RETRIES      3

enum mt_tx_status {
	MT_TX_SENT,     /* transmitted (first time) */
	MT_TX_ACKED,    /* acknowledged (explicitly or implicitly) */
	MT_TX_NO_ACK,   /* broadcast: nobody relayed it */
	MT_TX_FAILED,   /* direct message: no ACK, or a NAK */
};

/* A decoded packet for us (or a broadcast). */
struct mt_rx {
	struct mt_header h;
	struct mt_data d;
	uint8_t channel_index;
	bool pki;
	int16_t rssi;
	int8_t snr;
	uint8_t plain[MT_MAX_PAYLOAD]; /* d.payload points in here */
};

struct mt_router_ops {
	void (*transmit)(void *ctx, const uint8_t *frame, size_t len);
	void (*deliver)(void *ctx, const struct mt_rx *rx);
	void (*status)(void *ctx, uint32_t id, enum mt_tx_status status);
	uint32_t (*random)(void *ctx);
};

struct mt_txq_entry {
	bool used;
	uint32_t due;
	uint32_t from, id;  /* for cancelling */
	bool relay;         /* a rebroadcast, may be cancelled */
	uint8_t len;
	uint8_t frame[MT_MAX_FRAME];
};

struct mt_pending {
	bool used;
	uint32_t id, to;
	uint8_t retries;
	bool sent;
	uint32_t next;
	uint8_t len;
	uint8_t frame[MT_MAX_FRAME];
};

struct mt_router {
	const struct mt_router_ops *ops;
	void *ctx;
	uint32_t my_num;
	bool has_keys;
	uint8_t priv[32], pub[32];
	bool rebroadcast;

	struct mt_radio_params radio;
	struct mt_channel channels[MT_MAX_CHANNELS];
	int nchannels;
	struct mt_nodedb *db;

	struct {
		uint32_t from, id;
	} seen[MT_SEEN_LEN];
	int seen_pos;

	struct mt_txq_entry txq[MT_TXQ_LEN];
	struct mt_pending pending[MT_PENDING_LEN];
	uint32_t now;

	/* Statistics */
	uint32_t rx_packets, rx_dupes, rx_undecoded, tx_packets, tx_relayed, relay_cancelled;
};

void mt_router_init(struct mt_router *r, const struct mt_router_ops *ops, void *ctx,
		    uint32_t my_num, struct mt_nodedb *db);
/* Our X25519 key pair (enables PKI). */
void mt_router_set_keys(struct mt_router *r, const uint8_t priv[32]);
void mt_router_set_radio(struct mt_router *r, const struct mt_radio_params *p);

/*
 * Queue a packet. Returns its id, or 0 if the queue is full or the payload
 * too large. Status callbacks report what happens to it.
 */
uint32_t mt_router_send(struct mt_router *r, uint32_t to, uint8_t channel_index, uint32_t portnum,
			const void *payload, size_t len, bool want_ack, bool want_response,
			uint32_t request_id);

void mt_router_receive(struct mt_router *r, const uint8_t *frame, size_t len, int16_t rssi,
		       int8_t snr, uint32_t now);

/* Transmit what is due; returns the ms until the next event (UINT32_MAX: none). */
uint32_t mt_router_poll(struct mt_router *r, uint32_t now);

/* Retransmission timeout for a frame of len bytes (ms). */
uint32_t mt_router_retry_ms(const struct mt_router *r, size_t len);

#ifdef __cplusplus
}
#endif
