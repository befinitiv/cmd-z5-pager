/*
 * SX1262 as the mesh radio: raw LoRa packets with a free sync word
 * (Meshtastic 0x2b, which Zephyr's lora API cannot set).
 *
 * The radio receives continuously. A thread woken by DIO1 reads and clears
 * the interrupt flags, delivers received packets and reports the end of a
 * transmission or channel activity detection (CAD) to send(). send() listens
 * before it talks: a random backoff of 0-7 slots of 4 symbols, then CAD;
 * while a packet is being received (valid header) or CAD finds activity, it
 * backs off again.
 *
 * Wio-SX1262: TCXO on DIO3 (1.8 V), RF switch transmit path on DIO2 and
 * receive path on RF_SW (rx-enable-gpios), DC-DC regulator. The supply
 * (nPM1300 BUCK1) is switched on in init.
 *
 * Datasheet errata applied: 15.1 (modulation quality at 500 kHz),
 * 15.2 (PA clamping) and 15.4 (IQ polarity).
 */

#define DT_DRV_COMPAT cmdz5_sx1262

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <cmdz5/drivers/mesh_radio.h>

LOG_MODULE_REGISTER(sx1262, CONFIG_CMDZ5_RADIO_LOG_LEVEL);

/* Commands */
#define OP_SET_STANDBY           0x80
#define OP_SET_RX                0x82
#define OP_SET_TX                0x83
#define OP_SET_RF_FREQUENCY      0x86
#define OP_SET_CAD_PARAMS        0x88
#define OP_CALIBRATE             0x89
#define OP_SET_PACKET_TYPE       0x8A
#define OP_SET_MODULATION_PARAMS 0x8B
#define OP_SET_PACKET_PARAMS     0x8C
#define OP_SET_TX_PARAMS         0x8E
#define OP_SET_BUFFER_BASE       0x8F
#define OP_SET_PA_CONFIG         0x95
#define OP_SET_REGULATOR_MODE    0x96
#define OP_SET_DIO3_TCXO         0x97
#define OP_CALIBRATE_IMAGE       0x98
#define OP_SET_DIO2_RF_SWITCH    0x9D
#define OP_SET_CAD               0xC5
#define OP_SET_DIO_IRQ_PARAMS    0x08
#define OP_CLR_IRQ_STATUS        0x02
#define OP_CLR_DEVICE_ERRORS     0x07
#define OP_WRITE_REGISTER        0x0D
#define OP_WRITE_BUFFER          0x0E
#define OP_GET_IRQ_STATUS        0x12
#define OP_GET_RX_BUFFER_STATUS  0x13
#define OP_GET_PACKET_STATUS     0x14
#define OP_GET_RSSI_INST         0x15
#define OP_GET_DEVICE_ERRORS     0x17
#define OP_READ_REGISTER         0x1D
#define OP_READ_BUFFER           0x1E

/* Registers */
#define REG_IQ_POLARITY   0x0736
#define REG_SYNC_WORD     0x0740
#define REG_TX_MODULATION 0x0889
#define REG_RX_GAIN       0x08AC
#define REG_TX_CLAMP      0x08D8

#define SYNC_WORD_RESET 0x1424 /* 0x12, the value after reset */
#define RX_GAIN_BOOSTED 0x96

/* Interrupt flags */
#define IRQ_TX_DONE      BIT(0)
#define IRQ_RX_DONE      BIT(1)
#define IRQ_PREAMBLE     BIT(2)
#define IRQ_HEADER_VALID BIT(4)
#define IRQ_HEADER_ERR   BIT(5)
#define IRQ_CRC_ERR      BIT(6)
#define IRQ_CAD_DONE     BIT(7)
#define IRQ_CAD_DETECTED BIT(8)
#define IRQ_TIMEOUT      BIT(9)

#define IRQ_DIO1 (IRQ_TX_DONE | IRQ_RX_DONE | IRQ_HEADER_ERR | IRQ_CRC_ERR | IRQ_CAD_DONE | \
		  IRQ_TIMEOUT)
#define IRQ_ALL  (IRQ_DIO1 | IRQ_PREAMBLE | IRQ_HEADER_VALID | IRQ_CAD_DETECTED)

#define BUSY_TIMEOUT_MS 100
/* Listen before talk: backoff of 0..BACKOFF_SLOTS-1 slots of SLOT_SYMBOLS symbols. */
#define BACKOFF_SLOTS 8
#define SLOT_SYMBOLS  4
/* Send anyway after this many busy channel checks. */
#define LBT_MAX_TRIES 20
/* Noise floor: RSSI samples per reading, about 0.6 ms apart. */
#define NOISE_SAMPLES 32

enum mode {
	MODE_STANDBY,
	MODE_RX,
	MODE_CAD,
	MODE_TX,
};

struct sx1262_config {
	struct spi_dt_spec bus;
	struct gpio_dt_spec reset;
	struct gpio_dt_spec busy;
	struct gpio_dt_spec dio1;
	struct gpio_dt_spec rx_enable;
	const struct device *supply;
	bool dio2_tx_enable;
	bool tcxo;
	uint8_t tcxo_voltage;
	uint16_t tcxo_delay_ms;
};

struct sx1262_data {
	const struct device *dev;
	struct k_mutex op_lock;  /* one configure/send/receive at a time */
	struct k_mutex spi_lock; /* one command at a time */
	struct k_sem irq_sem;
	struct k_sem done_sem; /* end of TX or CAD */
	struct gpio_callback dio1_cb;
	atomic_t mode;
	uint16_t done_irq; /* flags that ended the TX or CAD */
	atomic_t irqs;     /* interrupt rounds handled, to notice traffic */
	bool rx_blocked;   /* receive path held off (noise test) */
	struct mesh_radio_config cfg;
	bool configured;
	mesh_radio_rx_cb cb;
	void *user;
	uint8_t rx_buf[255];
	struct k_thread thread;

	K_KERNEL_STACK_MEMBER(stack, CONFIG_CMDZ5_SX1262_THREAD_STACK_SIZE);
};

/* SPI ------------------------------------------------------------------------------ */

static int wait_busy(const struct device *dev)
{
	const struct sx1262_config *cfg = dev->config;
	k_timepoint_t end = sys_timepoint_calc(K_MSEC(BUSY_TIMEOUT_MS));
	int polls = 0;

	while (gpio_pin_get_dt(&cfg->busy) > 0) {
		if (sys_timepoint_expired(end)) {
			LOG_ERR("BUSY stuck high");
			return -ETIMEDOUT;
		}
		/* Most commands take a few microseconds, calibration a few milliseconds. */
		if (polls++ < 50) {
			k_busy_wait(10);
		} else {
			k_sleep(K_USEC(200));
		}
	}
	return 0;
}

/*
 * One command: hdr (opcode and parameters, for reads including the status
 * NOP), then either wlen bytes from wdata or rlen bytes into rdata.
 */
static int xfer(const struct device *dev, const uint8_t *hdr, size_t hlen, const uint8_t *wdata,
		size_t wlen, uint8_t *rdata, size_t rlen)
{
	/* EasyDMA reads only RAM: NOPs and the header are not taken from flash. */
	static uint8_t nops[255];
	const struct sx1262_config *cfg = dev->config;
	struct sx1262_data *data = dev->data;
	uint8_t h[12]; /* longest command: SetDioIrqParams, 9 bytes */
	struct spi_buf tx[2] = {
		{.buf = h, .len = hlen},
		{.buf = (uint8_t *)(wdata ? wdata : nops), .len = wdata ? wlen : rlen},
	};
	struct spi_buf rx[2] = {
		{.buf = NULL, .len = hlen},
		{.buf = rdata, .len = rlen},
	};
	const struct spi_buf_set tx_set = {.buffers = tx, .count = (wlen || rlen) ? 2 : 1};
	const struct spi_buf_set rx_set = {.buffers = rx, .count = 2};
	int ret;

	if (hlen > sizeof(h) || rlen > sizeof(nops)) {
		return -EINVAL;
	}
	memcpy(h, hdr, hlen);
	k_mutex_lock(&data->spi_lock, K_FOREVER);
	ret = wait_busy(dev);
	if (ret == 0) {
		ret = spi_transceive_dt(&cfg->bus, &tx_set, rlen ? &rx_set : NULL);
	}
	k_mutex_unlock(&data->spi_lock);
	return ret;
}

static int cmd(const struct device *dev, const uint8_t *buf, size_t len)
{
	return xfer(dev, buf, len, NULL, 0, NULL, 0);
}

#define CMD(dev, ...) cmd(dev, (const uint8_t[]){__VA_ARGS__}, sizeof((uint8_t[]){__VA_ARGS__}))

/* Run one step of a sequence; remember and log the first failure. */
#define STEP(ret, expr)                                                                    \
	do {                                                                               \
		int r_ = (expr);                                                           \
		if (r_ != 0 && (ret) == 0) {                                               \
			LOG_ERR("%s, line %d: %d", __func__, __LINE__, r_);                \
			(ret) = r_;                                                        \
		}                                                                          \
	} while (0)

static int cmd_read(const struct device *dev, uint8_t op, uint8_t *out, size_t len)
{
	const uint8_t hdr[] = {op, 0};

	return xfer(dev, hdr, sizeof(hdr), NULL, 0, out, len);
}

static int reg_write(const struct device *dev, uint16_t addr, const uint8_t *val, size_t len)
{
	const uint8_t hdr[] = {OP_WRITE_REGISTER, addr >> 8, addr};

	return xfer(dev, hdr, sizeof(hdr), val, len, NULL, 0);
}

static int reg_read(const struct device *dev, uint16_t addr, uint8_t *val, size_t len)
{
	const uint8_t hdr[] = {OP_READ_REGISTER, addr >> 8, addr, 0};

	return xfer(dev, hdr, sizeof(hdr), NULL, 0, val, len);
}

/* Read-modify-write of one register byte. */
static int reg_update(const struct device *dev, uint16_t addr, uint8_t clear, uint8_t set)
{
	uint8_t v;
	int ret = reg_read(dev, addr, &v, 1);

	if (ret == 0) {
		v = (v & ~clear) | set;
		ret = reg_write(dev, addr, &v, 1);
	}
	return ret;
}

static int irq_get(const struct device *dev, uint16_t *irq)
{
	uint8_t v[2];
	int ret = cmd_read(dev, OP_GET_IRQ_STATUS, v, sizeof(v));

	*irq = ret == 0 ? (v[0] << 8) | v[1] : 0;
	return ret;
}

static int irq_clear(const struct device *dev, uint16_t irq)
{
	return CMD(dev, OP_CLR_IRQ_STATUS, irq >> 8, irq);
}

/* Chip mode, command status, device errors and interrupt flags, for debugging. */
static void dump(const struct device *dev, const char *where)
{
	const struct sx1262_config *cfg = dev->config;
	const uint8_t hdr[] = {0xC0};
	uint8_t status = 0, err[2] = {0};
	uint16_t irq = 0;

	if (!IS_ENABLED(CONFIG_CMDZ5_RADIO_LOG_LEVEL_DBG)) {
		return;
	}
	xfer(dev, hdr, sizeof(hdr), NULL, 0, &status, 1);
	cmd_read(dev, OP_GET_DEVICE_ERRORS, err, sizeof(err));
	irq_get(dev, &irq);
	LOG_DBG("%s: mode %u, cmd %u, errors 0x%02x%02x, irq 0x%04x, BUSY %d, DIO1 %d", where,
		(status >> 4) & 7, (status >> 1) & 7, err[0], err[1], irq,
		gpio_pin_get_dt(&cfg->busy), gpio_pin_get_dt(&cfg->dio1));
}

/* Modes ---------------------------------------------------------------------------- */

static int standby(const struct device *dev)
{
	struct sx1262_data *data = dev->data;

	atomic_set(&data->mode, MODE_STANDBY);
	return CMD(dev, OP_SET_STANDBY, 0x00); /* STDBY_RC */
}

static void rx_switch(const struct device *dev, bool on)
{
	const struct sx1262_config *cfg = dev->config;
	struct sx1262_data *data = dev->data;

	if (cfg->rx_enable.port) {
		gpio_pin_set_dt(&cfg->rx_enable, on && !data->rx_blocked);
	}
}

static int packet_params(const struct device *dev, uint8_t len)
{
	struct sx1262_data *data = dev->data;
	uint16_t pre = data->cfg.preamble;

	/* Explicit header, CRC on, standard IQ. */
	return CMD(dev, OP_SET_PACKET_PARAMS, pre >> 8, pre, 0x00, len, 0x01, 0x00);
}

static int start_rx(const struct device *dev)
{
	struct sx1262_data *data = dev->data;
	int ret;

	rx_switch(dev, true);
	ret = packet_params(dev, 255);
	if (ret == 0) {
		atomic_set(&data->mode, MODE_RX);
		ret = CMD(dev, OP_SET_RX, 0xFF, 0xFF, 0xFF); /* continuous */
	}
	return ret;
}

/* Back to receiving if someone listens, otherwise standby. */
static int idle(const struct device *dev)
{
	struct sx1262_data *data = dev->data;

	if (data->cb && data->configured) {
		return start_rx(dev);
	}
	rx_switch(dev, false);
	return standby(dev);
}

static uint32_t symbol_us(const struct mesh_radio_config *c)
{
	return (uint32_t)(((uint64_t)1000000 << c->sf) / c->bw_hz);
}

/* Semtech time-on-air formula, explicit header, CRC on (as mt_airtime_ms). */
static uint32_t airtime_ms(const struct mesh_radio_config *c, size_t len)
{
	uint32_t tsym = symbol_us(c);
	int de = tsym >= 16000;
	int num = 8 * (int)len - 4 * c->sf + 28 + 16;
	int den = 4 * (c->sf - 2 * de);
	int payload = 8 + MAX(DIV_ROUND_UP(MAX(num, 0), den) * c->cr, 0);

	return ((c->preamble * 4 + 17 + payload * 4) * tsym / 4 + 999) / 1000;
}

/* Interrupt thread ----------------------------------------------------------------- */

static void dio1_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	struct sx1262_data *data = CONTAINER_OF(cb, struct sx1262_data, dio1_cb);

	ARG_UNUSED(port);
	ARG_UNUSED(pins);
	k_sem_give(&data->irq_sem);
}

static void receive_packet(const struct device *dev, uint16_t irq)
{
	struct sx1262_data *data = dev->data;
	uint8_t st[3];
	uint8_t hdr[3];
	uint8_t len;

	if (irq & (IRQ_CRC_ERR | IRQ_HEADER_ERR)) {
		LOG_DBG("packet with %s error", (irq & IRQ_CRC_ERR) ? "CRC" : "header");
		return;
	}
	if (cmd_read(dev, OP_GET_RX_BUFFER_STATUS, st, 2) != 0 || st[0] == 0) {
		return;
	}
	len = st[0];
	hdr[0] = OP_READ_BUFFER;
	hdr[1] = st[1];
	hdr[2] = 0;
	if (xfer(dev, hdr, sizeof(hdr), NULL, 0, data->rx_buf, len) != 0 ||
	    cmd_read(dev, OP_GET_PACKET_STATUS, st, 3) != 0) {
		return;
	}
	int16_t rssi = -(int16_t)st[0] / 2;
	int8_t snr = ((int8_t)st[1] + 2) / 4;
	mesh_radio_rx_cb cb = data->cb;

	LOG_DBG("rx %u bytes, RSSI %d dBm, SNR %d dB", len, rssi, snr);
	if (cb) {
		cb(dev, data->rx_buf, len, rssi, snr, data->user);
	}
}

static void irq_thread(void *p1, void *p2, void *p3)
{
	const struct device *dev = p1;
	const struct sx1262_config *cfg = dev->config;
	struct sx1262_data *data = dev->data;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		k_sem_take(&data->irq_sem, K_FOREVER);
		/* DIO1 stays high until all its flags are cleared. */
		do {
			uint16_t irq;

			if (irq_get(dev, &irq) != 0 || irq == 0) {
				break;
			}
			irq_clear(dev, irq);
			atomic_inc(&data->irqs);

			enum mode mode = atomic_get(&data->mode);

			if ((mode == MODE_TX && (irq & (IRQ_TX_DONE | IRQ_TIMEOUT))) ||
			    (mode == MODE_CAD && (irq & IRQ_CAD_DONE))) {
				data->done_irq = irq;
				k_sem_give(&data->done_sem);
			}
			if (irq & IRQ_RX_DONE) {
				receive_packet(dev, irq);
			}
		} while (gpio_pin_get_dt(&cfg->dio1) > 0);
	}
}

/* Listen before talk ------------------------------------------------------------- */

/* A packet is arriving: a valid header without RX done yet. */
static bool receiving(const struct device *dev)
{
	struct sx1262_data *data = dev->data;
	uint16_t irq;

	return atomic_get(&data->mode) == MODE_RX && irq_get(dev, &irq) == 0 &&
	       (irq & IRQ_HEADER_VALID);
}

/* Channel activity detection; returns true if the channel is busy. */
static bool cad_busy(const struct device *dev)
{
	struct sx1262_data *data = dev->data;
	uint32_t timeout_ms = 2 * 4 * symbol_us(&data->cfg) / 1000 + 50;
	bool busy;

	standby(dev);
	k_sem_reset(&data->done_sem);
	atomic_set(&data->mode, MODE_CAD);
	if (CMD(dev, OP_SET_CAD) != 0) {
		return false;
	}
	if (k_sem_take(&data->done_sem, K_MSEC(timeout_ms)) != 0) {
		LOG_WRN("CAD timeout");
		dump(dev, "CAD timeout");
		busy = false;
	} else {
		busy = data->done_irq & IRQ_CAD_DETECTED;
	}
	return busy;
}

static void listen_before_talk(const struct device *dev)
{
	struct sx1262_data *data = dev->data;
	uint32_t slot_ms = MAX(SLOT_SYMBOLS * symbol_us(&data->cfg) / 1000, 1);

	for (int tries = 0; tries < LBT_MAX_TRIES; tries++) {
		k_msleep(sys_rand32_get() % BACKOFF_SLOTS * slot_ms);
		if (receiving(dev)) {
			continue;
		}
		bool busy = cad_busy(dev);

		idle(dev);
		if (!busy) {
			return;
		}
		LOG_DBG("channel busy");
	}
	LOG_WRN("channel busy, sending anyway");
}

/* API ------------------------------------------------------------------------------ */

static uint8_t bw_code(uint32_t bw_hz)
{
	static const struct {
		uint32_t hz;
		uint8_t code;
	} bws[] = {
		{7810, 0x00},   {10420, 0x08},  {15630, 0x01},  {20830, 0x09},  {31250, 0x02},
		{41670, 0x0A},  {62500, 0x03},  {125000, 0x04}, {250000, 0x05}, {500000, 0x06},
	};

	for (size_t i = 0; i < ARRAY_SIZE(bws); i++) {
		if (bw_hz >= bws[i].hz - 100 && bw_hz <= bws[i].hz + 100) {
			return bws[i].code;
		}
	}
	return 0xFF;
}

static int apply(const struct device *dev, const struct mesh_radio_config *c)
{
	uint8_t bw = bw_code(c->bw_hz);
	uint32_t frf = ((uint64_t)c->freq_hz << 25) / 32000000;
	int8_t power = CLAMP(c->power_dbm, -9, 22);
	uint8_t ldro = symbol_us(c) >= 16000;
	uint8_t sync[] = {(c->sync_word & 0xF0) | 0x04, ((c->sync_word & 0x0F) << 4) | 0x04};
	uint8_t img[2];
	int ret = 0;

	if (bw == 0xFF || c->sf < 5 || c->sf > 12 || c->cr < 5 || c->cr > 8) {
		return -EINVAL;
	}
	/* Image calibration for the band. */
	if (c->freq_hz > 900000000) {
		img[0] = 0xE1, img[1] = 0xE9;
	} else if (c->freq_hz > 850000000) {
		img[0] = 0xD7, img[1] = 0xDB;
	} else if (c->freq_hz > 770000000) {
		img[0] = 0xC1, img[1] = 0xC5;
	} else if (c->freq_hz > 460000000) {
		img[0] = 0x75, img[1] = 0x81;
	} else {
		img[0] = 0x6B, img[1] = 0x6F;
	}

	STEP(ret, standby(dev));
	STEP(ret, CMD(dev, OP_CALIBRATE_IMAGE, img[0], img[1]));
	STEP(ret, CMD(dev, OP_SET_RF_FREQUENCY, frf >> 24, frf >> 16, frf >> 8, frf));
	/* SX1262 high-power PA, up to +22 dBm; ramp 200 us. */
	STEP(ret, CMD(dev, OP_SET_PA_CONFIG, 0x04, 0x07, 0x00, 0x01));
	STEP(ret, CMD(dev, OP_SET_TX_PARAMS, (uint8_t)power, 0x04));
	STEP(ret, CMD(dev, OP_SET_MODULATION_PARAMS, c->sf, bw, c->cr - 4, ldro));
	/* Errata 15.1: modulation quality register, cleared only at 500 kHz. */
	STEP(ret, reg_update(dev, REG_TX_MODULATION, BIT(2), bw == 0x06 ? 0 : BIT(2)));
	STEP(ret, reg_write(dev, REG_SYNC_WORD, sync, sizeof(sync)));
	/* CAD over 2 symbols, as Meshtastic's slot time assumes. */
	STEP(ret, CMD(dev, OP_SET_CAD_PARAMS, 0x01, c->sf + 13, 10, 0x00, 0, 0, 0));
	if (ret != 0) {
		return -EIO;
	}
	if (c->power_dbm > 22) {
		LOG_DBG("power limited to 22 dBm (asked %d)", c->power_dbm);
	}
	LOG_INF("%u.%03u MHz, SF%u, %u kHz, CR4/%u, sync 0x%02x, %d dBm", c->freq_hz / 1000000,
		(c->freq_hz / 1000) % 1000, c->sf, c->bw_hz / 1000, c->cr, c->sync_word, power);
	return 0;
}

static int sx1262_configure(const struct device *dev, const struct mesh_radio_config *c)
{
	struct sx1262_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->op_lock, K_FOREVER);
	data->cfg = *c;
	ret = apply(dev, c);
	dump(dev, "configured");
	data->configured = ret == 0;
	idle(dev);
	k_mutex_unlock(&data->op_lock);
	return ret;
}

static int sx1262_send(const struct device *dev, const uint8_t *buf, size_t len)
{
	const struct sx1262_config *cfg = dev->config;
	struct sx1262_data *data = dev->data;
	const uint8_t hdr[] = {OP_WRITE_BUFFER, 0};
	int ret;

	if (len == 0 || len > 255) {
		return -EINVAL;
	}
	k_mutex_lock(&data->op_lock, K_FOREVER);
	if (!data->configured) {
		k_mutex_unlock(&data->op_lock);
		return -EIO;
	}
	listen_before_talk(dev);

	uint32_t air = airtime_ms(&data->cfg, len);

	standby(dev);
	rx_switch(dev, false);
	ret = xfer(dev, hdr, sizeof(hdr), buf, len, NULL, 0);
	ret |= packet_params(dev, len);
	k_sem_reset(&data->done_sem);
	atomic_set(&data->mode, MODE_TX);
	ret |= CMD(dev, OP_SET_TX, 0, 0, 0); /* no timeout */
	if (ret != 0) {
		ret = -EIO;
	} else if (k_sem_take(&data->done_sem, K_MSEC(2 * air + 100)) != 0) {
		LOG_ERR("TX timeout (%u bytes, %u ms on air, DIO1 %d)", len, air,
			gpio_pin_get_dt(&cfg->dio1));
		dump(dev, "TX timeout");
		ret = -ETIMEDOUT;
	} else if (!(data->done_irq & IRQ_TX_DONE)) {
		ret = -EIO;
	} else {
		LOG_DBG("tx %u bytes, %u ms", len, air);
	}
	idle(dev);
	k_mutex_unlock(&data->op_lock);
	return ret;
}

static int sx1262_receive(const struct device *dev, mesh_radio_rx_cb cb, void *user_data)
{
	struct sx1262_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->op_lock, K_FOREVER);
	data->user = user_data;
	data->cb = cb;
	ret = idle(dev);
	k_mutex_unlock(&data->op_lock);
	return ret;
}

/*
 * Noise floor: the median of instantaneous RSSI samples while receiving. A
 * reading that overlaps a packet (a preamble, a header or any interrupt seen
 * meanwhile) is refused with -EAGAIN; while sending or busy -EBUSY.
 */
static int sx1262_noise(const struct device *dev, int16_t *dbm)
{
	struct sx1262_data *data = dev->data;
	uint8_t s[NOISE_SAMPLES];
	uint16_t irq = 0;
	int ret = 0;

	if (k_mutex_lock(&data->op_lock, K_NO_WAIT) != 0) {
		return -EBUSY;
	}
	if (atomic_get(&data->mode) != MODE_RX) {
		k_mutex_unlock(&data->op_lock);
		return -EBUSY;
	}
	atomic_val_t irqs = atomic_get(&data->irqs);

	irq_clear(dev, IRQ_PREAMBLE);
	for (int i = 0; i < NOISE_SAMPLES && ret == 0; i++) {
		ret = cmd_read(dev, OP_GET_RSSI_INST, &s[i], 1);
		k_busy_wait(500);
	}
	ret = ret ?: irq_get(dev, &irq);
	k_mutex_unlock(&data->op_lock);
	if (ret != 0) {
		return -EIO;
	}
	if ((irq & (IRQ_PREAMBLE | IRQ_HEADER_VALID)) || atomic_get(&data->irqs) != irqs) {
		return -EAGAIN;
	}
	/* Insertion sort; the value is -RSSI in half dB. */
	for (int i = 1; i < NOISE_SAMPLES; i++) {
		uint8_t v = s[i];
		int j = i;

		for (; j > 0 && s[j - 1] > v; j--) {
			s[j] = s[j - 1];
		}
		s[j] = v;
	}
	*dbm = -(int16_t)((s[NOISE_SAMPLES / 2] + 1) / 2);
	return 0;
}

/* SetRegulatorMode in standby, then back to receiving. */
static int sx1262_regulator_dcdc(const struct device *dev, bool dcdc)
{
	struct sx1262_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->op_lock, K_FOREVER);
	standby(dev);
	ret = CMD(dev, OP_SET_REGULATOR_MODE, dcdc ? 0x01 : 0x00);
	ret |= idle(dev);
	k_mutex_unlock(&data->op_lock);
	return ret ? -EIO : 0;
}

/* NRESET has a pull-up in the SX1262: undriven it stays high. */
static int sx1262_reset_released(const struct device *dev, bool released)
{
	const struct sx1262_config *cfg = dev->config;

	return gpio_pin_configure_dt(&cfg->reset,
				     released ? GPIO_INPUT : GPIO_OUTPUT_INACTIVE);
}

static int sx1262_rx_path_blocked(const struct device *dev, bool blocked)
{
	const struct sx1262_config *cfg = dev->config;
	struct sx1262_data *data = dev->data;

	if (!cfg->rx_enable.port) {
		return -ENOTSUP;
	}
	k_mutex_lock(&data->op_lock, K_FOREVER);
	data->rx_blocked = blocked;
	rx_switch(dev, atomic_get(&data->mode) == MODE_RX);
	k_mutex_unlock(&data->op_lock);
	return 0;
}

static const struct mesh_radio_api sx1262_api = {
	.configure = sx1262_configure,
	.send = sx1262_send,
	.receive = sx1262_receive,
	.noise = sx1262_noise,
	.regulator_dcdc = sx1262_regulator_dcdc,
	.reset_released = sx1262_reset_released,
	.rx_path_blocked = sx1262_rx_path_blocked,
};

/* Init ----------------------------------------------------------------------------- */

static int chip_setup(const struct device *dev)
{
	const struct sx1262_config *cfg = dev->config;
	uint8_t err[2];
	int ret = 0;

	dump(dev, "reset");
	STEP(ret, standby(dev));
	if (cfg->tcxo) {
		uint32_t delay = cfg->tcxo_delay_ms * 64; /* units of 15.625 us */

		STEP(ret, CMD(dev, OP_SET_DIO3_TCXO, cfg->tcxo_voltage, delay >> 16, delay >> 8,
			   delay));
	}
	STEP(ret, CMD(dev, OP_CALIBRATE, 0x7F)); /* all blocks, with the TCXO running */
	dump(dev, "calibrate");
	STEP(ret, cmd_read(dev, OP_GET_DEVICE_ERRORS, err, sizeof(err)));
	if (ret == 0 && (err[0] || err[1])) {
		/* XOSC_START_ERR is expected once after the TCXO is set up. */
		LOG_DBG("device errors 0x%02x%02x", err[0], err[1]);
	}
	STEP(ret, CMD(dev, OP_CLR_DEVICE_ERRORS, 0, 0));
	STEP(ret, CMD(dev, OP_SET_REGULATOR_MODE, 0x01)); /* DC-DC */
	STEP(ret, CMD(dev, OP_SET_BUFFER_BASE, 0, 0));
	STEP(ret, CMD(dev, OP_SET_PACKET_TYPE, 0x01)); /* LoRa */
	if (cfg->dio2_tx_enable) {
		STEP(ret, CMD(dev, OP_SET_DIO2_RF_SWITCH, 0x01));
	}
	STEP(ret, CMD(dev, OP_SET_DIO_IRQ_PARAMS, IRQ_ALL >> 8, IRQ_ALL & 0xFF, IRQ_DIO1 >> 8,
		   IRQ_DIO1 & 0xFF, 0, 0, 0, 0));
	STEP(ret, reg_write(dev, REG_RX_GAIN, (const uint8_t[]){RX_GAIN_BOOSTED}, 1));
	/* Errata 15.2: PA clamping against antenna mismatch. */
	STEP(ret, reg_update(dev, REG_TX_CLAMP, 0, 0x1E));
	/* Errata 15.4: IQ polarity bit for standard IQ. */
	STEP(ret, reg_update(dev, REG_IQ_POLARITY, 0, BIT(2)));
	STEP(ret, irq_clear(dev, 0xFFFF));
	STEP(ret, cmd_read(dev, OP_GET_DEVICE_ERRORS, err, sizeof(err)));
	if (ret == 0 && (err[0] || err[1])) {
		LOG_WRN("device errors 0x%02x%02x after setup", err[0], err[1]);
	}
	return ret ? -EIO : 0;
}

static int sx1262_init(const struct device *dev)
{
	const struct sx1262_config *cfg = dev->config;
	struct sx1262_data *data = dev->data;
	uint8_t sync[2];
	int ret;

	data->dev = dev;
	k_mutex_init(&data->op_lock);
	k_mutex_init(&data->spi_lock);
	k_sem_init(&data->irq_sem, 0, 1);
	k_sem_init(&data->done_sem, 0, 1);

	if (!spi_is_ready_dt(&cfg->bus) || !gpio_is_ready_dt(&cfg->reset) ||
	    !gpio_is_ready_dt(&cfg->busy) || !gpio_is_ready_dt(&cfg->dio1) ||
	    (cfg->supply && !device_is_ready(cfg->supply))) {
		LOG_ERR("bus, GPIO or supply not ready");
		return -ENODEV;
	}
	/* Hold the radio in reset while its supply comes up. */
	gpio_pin_configure_dt(&cfg->reset, GPIO_OUTPUT_ACTIVE);
	gpio_pin_configure_dt(&cfg->busy, GPIO_INPUT);
	gpio_pin_configure_dt(&cfg->dio1, GPIO_INPUT);
	if (cfg->rx_enable.port) {
		gpio_pin_configure_dt(&cfg->rx_enable, GPIO_OUTPUT_INACTIVE);
	}
	if (cfg->supply) {
		int32_t uv = 0;

		ret = regulator_enable(cfg->supply);
		if (ret < 0) {
			LOG_ERR("supply not switched on (%d)", ret);
			return ret;
		}
		regulator_get_voltage(cfg->supply, &uv);
		LOG_DBG("supply %d mV", uv / 1000);
		k_msleep(10);
	}
	gpio_pin_set_dt(&cfg->reset, 0);
	k_msleep(5);

	/* The sync word register holds 0x1424 after reset: proof that the chip answers. */
	ret = reg_read(dev, REG_SYNC_WORD, sync, sizeof(sync));
	if (ret != 0 || ((sync[0] << 8) | sync[1]) != SYNC_WORD_RESET) {
		LOG_ERR("SX1262 not found (%d, sync word register 0x%02x%02x)", ret, sync[0],
			sync[1]);
		gpio_pin_set_dt(&cfg->reset, 1);
		if (cfg->supply) {
			regulator_disable(cfg->supply);
		}
		return -ENODEV;
	}
	ret = chip_setup(dev);
	if (ret != 0) {
		LOG_ERR("setup failed");
		return ret;
	}

	gpio_init_callback(&data->dio1_cb, dio1_isr, BIT(cfg->dio1.pin));
	ret = gpio_add_callback_dt(&cfg->dio1, &data->dio1_cb);
	if (ret == 0) {
		ret = gpio_pin_interrupt_configure_dt(&cfg->dio1, GPIO_INT_EDGE_TO_ACTIVE);
	}
	if (ret != 0) {
		LOG_ERR("DIO1 interrupt not set up (%d)", ret);
		return ret;
	}
	k_thread_create(&data->thread, data->stack, K_KERNEL_STACK_SIZEOF(data->stack),
			irq_thread, (void *)dev, NULL, NULL, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
	k_thread_name_set(&data->thread, dev->name);
	LOG_INF("SX1262 ready");
	return 0;
}

#define SX1262_DEFINE(n)                                                                     \
	static const struct sx1262_config sx1262_config_##n = {                              \
		.bus = SPI_DT_SPEC_INST_GET(n, SPI_WORD_SET(8) | SPI_TRANSFER_MSB |          \
						       SPI_OP_MODE_MASTER),                  \
		.reset = GPIO_DT_SPEC_INST_GET(n, reset_gpios),                              \
		.busy = GPIO_DT_SPEC_INST_GET(n, busy_gpios),                                \
		.dio1 = GPIO_DT_SPEC_INST_GET(n, dio1_gpios),                                \
		.rx_enable = GPIO_DT_SPEC_INST_GET_OR(n, rx_enable_gpios, {0}),              \
		.supply = COND_CODE_1(DT_INST_NODE_HAS_PROP(n, power_supply),                \
				      (DEVICE_DT_GET(DT_INST_PHANDLE(n, power_supply))), (NULL)), \
		.dio2_tx_enable = DT_INST_PROP(n, dio2_tx_enable),                           \
		.tcxo = DT_INST_NODE_HAS_PROP(n, dio3_tcxo_voltage),                         \
		.tcxo_voltage = DT_INST_PROP_OR(n, dio3_tcxo_voltage, 0),                    \
		.tcxo_delay_ms = DT_INST_PROP(n, tcxo_power_startup_delay_ms),               \
	};                                                                                   \
	static struct sx1262_data sx1262_data_##n;                                           \
	DEVICE_DT_INST_DEFINE(n, sx1262_init, NULL, &sx1262_data_##n, &sx1262_config_##n,     \
			      POST_KERNEL, CONFIG_CMDZ5_SX1262_INIT_PRIORITY, &sx1262_api);

DT_INST_FOREACH_STATUS_OKAY(SX1262_DEFINE)
