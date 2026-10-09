/*
 * Host side of the emulated radio: UDP multicast datagrams between emulator
 * instances on this machine (loopback interface). Standard C types only.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/* Open the socket; returns 0 or -1 (radio unavailable). */
int radio_bottom_open(uint16_t port);
int radio_bottom_send(const uint8_t *data, size_t len);
/* Non-blocking receive; returns the length, 0 if nothing is waiting. */
int radio_bottom_recv(uint8_t *data, size_t size);
/* Host random bytes (getrandom). */
void radio_bottom_random(void *buf, size_t len);
