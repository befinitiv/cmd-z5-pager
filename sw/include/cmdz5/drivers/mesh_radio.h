/*
 * LoRa radio for the mesh: raw packets with a free sync word (Meshtastic uses
 * 0x2b, which the Zephyr lora API cannot set). Implemented by the emulator
 * (UDP multicast between emulator instances) and, from the hardware
 * bring-up on, by an SX1262 driver.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

struct mesh_radio_config {
	uint32_t freq_hz;
	uint32_t bw_hz;
	uint8_t sf;
	uint8_t cr;        /* 5..8 = 4/5..4/8 */
	uint8_t sync_word;
	uint16_t preamble;
	int8_t power_dbm;
};

/* Called from the driver's thread for every received packet. */
typedef void (*mesh_radio_rx_cb)(const struct device *dev, const uint8_t *data, size_t len,
				 int16_t rssi, int8_t snr, void *user_data);

struct mesh_radio_api {
	int (*configure)(const struct device *dev, const struct mesh_radio_config *cfg);
	/* Blocks until the packet is sent (its time on air). */
	int (*send)(const struct device *dev, const uint8_t *data, size_t len);
	/* Receive continuously (cb = NULL stops). Reception pauses while sending. */
	int (*receive)(const struct device *dev, mesh_radio_rx_cb cb, void *user_data);
};

static inline int mesh_radio_configure(const struct device *dev,
				       const struct mesh_radio_config *cfg)
{
	return ((const struct mesh_radio_api *)dev->api)->configure(dev, cfg);
}

static inline int mesh_radio_send(const struct device *dev, const uint8_t *data, size_t len)
{
	return ((const struct mesh_radio_api *)dev->api)->send(dev, data, len);
}

static inline int mesh_radio_receive(const struct device *dev, mesh_radio_rx_cb cb,
				     void *user_data)
{
	return ((const struct mesh_radio_api *)dev->api)->receive(dev, cb, user_data);
}
