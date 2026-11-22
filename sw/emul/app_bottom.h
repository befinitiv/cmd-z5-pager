/*
 * Host side of the companion app link: a TCP server on localhost for one
 * client at a time. Standard C types only.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/* Listen on 127.0.0.1:port; returns 0 or -1. */
int app_bottom_listen(uint16_t port);
/* Accept a waiting client: 1 if one connected now, 0 if not. */
int app_bottom_accept(void);
/* Non-blocking receive: the length, 0 if nothing is waiting, -1 if the client left. */
int app_bottom_recv(uint8_t *data, size_t size);
/* Send all; -1 if the client left. */
int app_bottom_send(const uint8_t *data, size_t len);
