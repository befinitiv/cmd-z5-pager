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
