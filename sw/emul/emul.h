/* Emulator services for the application (only with CONFIG_CMDZ5_EMUL). */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Random bytes from the host (the emulated entropy source is deterministic). */
void emul_random(void *buf, size_t len);
/* Node number given with --node-num, or 0. */
uint32_t emul_node_num(void);
/* Radio state for the simulated battery: receiving, and transmitting. */
bool emul_radio_rx(void);
bool emul_radio_tx(void);
