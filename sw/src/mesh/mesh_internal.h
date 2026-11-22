/*
 * Between the mesh service (mesh.c: Meshtastic, the node database, channels
 * and the public API) and its MeshCore mode (meshcore.c).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/settings/settings.h>

#include "mesh.h"

/* meshcore.c ------------------------------------------------------------------------ */

/*
 * Start, after its settings (subtree "mc", stored contacts going into the
 * node database) are loaded. 0 or -errno.
 */
int meshcore_start(const struct device *radio);
/* Names, channels or MeshCore settings changed. */
void meshcore_reconfig(void);
uint32_t meshcore_my_num(void);
/* Send a stored outgoing message (local id). */
int meshcore_send(uint16_t local_id);
void meshcore_get_stats(struct mesh_stats *s);
size_t meshcore_text_max(uint32_t conv);
int meshcore_node_lines(const struct mt_node *n, char lines[][32], int max);
void meshcore_send_advert(void);
/* A companion or room server (not a repeater or sensor). */
bool meshcore_person(const struct mt_node *n);
/* A node left the node database (replaced by a new one). */
void meshcore_node_evicted(const struct mt_node *n);
/* How long to keep a node (nodedb.h): favourites 2, people 1, others 0. */
int meshcore_rank(const struct mt_node *n);
/* The stored contacts into the node database (before the "mc" settings). */
void meshcore_load_contacts(void);

/*
 * For the companion protocol (companion.c), with the mesh lock held (from
 * the MeshCore thread, unless noted).
 */
struct mc_app_contact {
	uint8_t pub[32];
	char name[32];
	uint8_t type; /* enum mc_adv_type */
	uint8_t flags;
	uint8_t path_len; /* MC_PATH_UNKNOWN: flood */
	uint8_t path[64];
	uint32_t advert_ts;
	int32_t lat_e6, lon_e6;
	uint32_t lastmod;
};

/* A command frame from the app (any thread): run in the MeshCore thread. 0 or -EBUSY. */
int meshcore_app_frame(const uint8_t *frame, size_t len);
const uint8_t *meshcore_pub_key(void);
/* A MeshCore contact: a node with a public key from an advert. */
bool meshcore_is_contact(const struct mt_node *n);
void meshcore_contact_info(const struct mt_node *n, struct mc_app_contact *c);
/* The contact whose key starts with these bytes, or NULL. */
struct mt_node *meshcore_find_key(const uint8_t *key, size_t len);
/* Add or change a contact; false if the key is not usable. */
bool meshcore_contact_set(const struct mc_app_contact *c);
void meshcore_contact_remove(uint32_t num);
void meshcore_reset_path(uint32_t num);
/* An advert (a contact shared as a packet): verify and add. */
bool meshcore_import(const uint8_t *packet, size_t len);
/* Our advert as a packet (flood route); returns its length. */
int meshcore_export_self(uint8_t *packet);
void meshcore_app_advert(bool flood);
/*
 * One attempt of a direct message from the app: also stored as our message
 * (attempts of the same message, by its time, are one). Returns the id, or 0.
 */
uint32_t meshcore_app_send(const struct mt_node *n, uint32_t timestamp, uint8_t attempt,
			   const char *text, uint32_t *ack, uint32_t *timeout_ms, bool *flood);
/* Flood scope for the app's packets: a key, none (unscoped), or the default (both false). */
void meshcore_app_scope(const uint8_t *key, bool unscoped);
/* Our timestamps: the clock, else a stand-in that keeps rising. */
uint32_t meshcore_time(void);
struct mc_mesh;
const struct mc_mesh *meshcore_mesh(void);
/* The MeshCore client, to send through (lock held, MeshCore thread). */
struct mc_mesh *meshcore_lib(void);
struct mc_contact;
/* A contact's key, shared secret and route, for sending. */
void meshcore_contact_of(const struct mt_node *n, struct mc_contact *c);
/* A new timestamp of ours (rising), e.g. a request's tag. */
uint32_t meshcore_next_timestamp(void);
/* A radio preset's parameters (any thread); false if there is no such preset. */
bool meshcore_preset(int i, uint32_t *freq_hz, uint32_t *bw_hz, uint8_t *sf, uint8_t *cr);
/* companion.c: its stored settings ("mc/app"). */
int companion_prefs_read(size_t len, settings_read_cb read_cb, void *cb_arg);

/* mesh.c ------------------------------------------------------------------------------ */

/* Read the stored private channels (the active protocol's). */
int mesh_channels_read(size_t len, settings_read_cb read_cb, void *cb_arg);
/* Copy of the channel slots (slot 0 unused). */
void mesh_channels_get(struct mesh_channel out[MESH_CHANNELS]);
/* Give this boot's messages their time once the clock is set (call regularly). */
void mesh_date_messages(void);
/* Unix time if the clock is set, else 0; and the mark for dating it later. */
uint32_t mesh_now_time(void);
uint32_t mesh_uptime_mark(void);
void mesh_notify(enum mesh_event e);
