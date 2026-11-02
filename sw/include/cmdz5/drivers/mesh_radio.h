/*
 * LoRa radio for the mesh: raw packets with a free sync word (Meshtastic uses
 * 0x2b, which the Zephyr lora API cannot set). Implemented by the emulator
 * (UDP multicast between emulator instances) and, from the hardware
 * bring-up on, by an SX1262 driver.
 */

#pragma once

#include <errno.h>
#include <stdbool.h>
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
	/*
	 * Optional: the noise floor in dBm, measured while receiving and nothing
	 * arrives. -EAGAIN if a packet came in meanwhile, -EBUSY if not receiving.
	 */
	int (*noise)(const struct device *dev, int16_t *dbm);
	/* Optional: the radio chip's own supply regulator, DC/DC or LDO (tests). */
	int (*regulator_dcdc)(const struct device *dev, bool dcdc);
	/*
	 * Optional: stop driving the radio's reset line, which then stays high by
	 * the chip's own pull-up (tests: noise carried in on the line).
	 */
	int (*reset_released)(const struct device *dev, bool released);
	/*
	 * Optional: keep the RF switch's receive path off while receiving (tests:
	 * noise that enters before or after the switch). Reception stops working.
	 */
	int (*rx_path_blocked)(const struct device *dev, bool blocked);
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

static inline int mesh_radio_noise(const struct device *dev, int16_t *dbm)
{
	const struct mesh_radio_api *api = dev->api;

	return api->noise ? api->noise(dev, dbm) : -ENOSYS;
}

static inline int mesh_radio_reset_released(const struct device *dev, bool released)
{
	const struct mesh_radio_api *api = dev->api;

	return api->reset_released ? api->reset_released(dev, released) : -ENOSYS;
}

static inline int mesh_radio_rx_path_blocked(const struct device *dev, bool blocked)
{
	const struct mesh_radio_api *api = dev->api;

	return api->rx_path_blocked ? api->rx_path_blocked(dev, blocked) : -ENOSYS;
}

static inline int mesh_radio_regulator_dcdc(const struct device *dev, bool dcdc)
{
	const struct mesh_radio_api *api = dev->api;

	return api->regulator_dcdc ? api->regulator_dcdc(dev, dcdc) : -ENOSYS;
}
