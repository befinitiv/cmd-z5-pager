/* Bluetooth LE link for the MeshCore app: see ble.h. */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "app.h"
#include "ble.h"
#include "mesh/companion.h"
#include "mesh/mesh.h"

LOG_MODULE_REGISTER(ble, CONFIG_CMDZ5_APP_LOG_LEVEL);

static bool enabled;

static int ble_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, "on") == 0 && len == sizeof(enabled)) {
		return read_cb(cb_arg, &enabled, len) == (ssize_t)len ? 0 : -EIO;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(ble, "ble", NULL, ble_set, NULL, NULL);

bool ble_enabled(void)
{
	return enabled;
}

#ifdef CONFIG_BT

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#define NAME_PREFIX "MeshCore-"
/* Notifications in flight before waiting for one to be sent. */
#define TX_IN_FLIGHT 2

#define NUS_UUID BT_UUID_128_ENCODE(0x6e400001, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)
static const struct bt_uuid_128 nus_uuid = BT_UUID_INIT_128(NUS_UUID);
static const struct bt_uuid_128 rx_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x6e400002, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e));
static const struct bt_uuid_128 tx_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x6e400003, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e));

static bool started;    /* Bluetooth is up */
static bool available;  /* MeshCore mode */
static struct bt_conn *conn;
static bool notify_on;
static atomic_t passkey = ATOMIC_INIT(-1);

static K_SEM_DEFINE(tx_wake, 0, 1);
static K_SEM_DEFINE(tx_room, TX_IN_FLIGHT, TX_IN_FLIGHT);

static void post(enum ble_event e)
{
	struct ui_event ev = {.type = APP_EV_BLE, .value = e};

	app_post(&ev);
}

/* GATT ---------------------------------------------------------------------------- */

static ssize_t on_write(struct bt_conn *c, const struct bt_gatt_attr *attr, const void *buf,
			uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(c);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	/* One write is one frame. */
	if (companion_put(buf, len) != 0) {
		LOG_WRN("app frame dropped (%u bytes)", len);
	}
	return len;
}

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	notify_on = value == BT_GATT_CCC_NOTIFY;
	companion_connected(notify_on);
	k_sem_give(&tx_wake);
	post(BLE_EV_CONNECTION);
}

/* Both need an encrypted link with an authenticated (passkey) pairing. */
BT_GATT_SERVICE_DEFINE(nus_svc,
	BT_GATT_PRIMARY_SERVICE(&nus_uuid),
	BT_GATT_CHARACTERISTIC(&rx_uuid.uuid, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE_AUTHEN, NULL, on_write, NULL),
	BT_GATT_CHARACTERISTIC(&tx_uuid.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL, NULL,
			       NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_AUTHEN),
);

static void notified(struct bt_conn *c, void *user_data)
{
	ARG_UNUSED(c);
	ARG_UNUSED(user_data);
	k_sem_give(&tx_room);
}

static void wake(void)
{
	k_sem_give(&tx_wake);
}

/* Frames for the app, one notification each. */
static void tx_thread(void *p1, void *p2, void *p3)
{
	static uint8_t buf[COMPANION_FRAME_MAX];

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (;;) {
		k_sem_take(&tx_wake, K_FOREVER);
		while (conn && notify_on) {
			struct bt_gatt_notify_params params = {
				.attr = &nus_svc.attrs[4],
				.data = buf,
				.func = notified,
			};
			int len = companion_get(buf);
			int ret;

			if (len <= 0) {
				break;
			}
			if (len > bt_gatt_get_mtu(conn) - 3) {
				LOG_WRN("frame %02x (%d bytes) too long for MTU %u", buf[0], len,
					bt_gatt_get_mtu(conn));
				continue;
			}
			if (k_sem_take(&tx_room, K_SECONDS(5)) != 0) {
				LOG_WRN("notifications stuck");
				k_sem_reset(&tx_room);
				for (int i = 0; i < TX_IN_FLIGHT; i++) {
					k_sem_give(&tx_room);
				}
			}
			params.len = len;
			ret = bt_gatt_notify_cb(conn, &params);
			if (ret != 0) {
				LOG_WRN("notify failed (%d)", ret);
				k_sem_give(&tx_room);
			}
		}
	}
}

K_THREAD_DEFINE(ble_tx, 1536, tx_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(7), 0, 0);

/* Advertising ------------------------------------------------------------------------ */

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, NUS_UUID),
};

static void advertise(struct k_work *work)
{
	static char name[CONFIG_BT_DEVICE_NAME_MAX + 1];
	struct mesh_config mc;
	struct bt_data sd[1];
	int ret;

	ARG_UNUSED(work);
	if (!started || !enabled || conn) {
		return;
	}
	/* The scan response holds up to 29 bytes of name. */
	mesh_get_config(&mc);
	strcpy(name, NAME_PREFIX);
	strncat(name, mc.long_name, 29 - strlen(NAME_PREFIX));
	bt_set_name(name);
	sd[0] = (struct bt_data)BT_DATA(BT_DATA_NAME_COMPLETE, name, strlen(name));
	ret = bt_le_adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, BT_GAP_ADV_FAST_INT_MIN_2,
					      BT_GAP_ADV_FAST_INT_MAX_2, NULL),
			      ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (ret != 0 && ret != -EALREADY) {
		LOG_WRN("advertising failed (%d)", ret);
	} else {
		LOG_INF("advertising as \"%s\"", name);
	}
}

static K_WORK_DEFINE(adv_work, advertise);

/* Connections and pairing --------------------------------------------------------------- */

static void connected(struct bt_conn *c, uint8_t err)
{
	if (err) {
		LOG_WRN("connection failed (0x%02x)", err);
		return;
	}
	if (conn) {
		bt_conn_disconnect(c, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}
	conn = bt_conn_ref(c);
	LOG_INF("phone connected");
	/* Ask for pairing (or the stored keys) right away. */
	bt_conn_set_security(c, BT_SECURITY_L3);
	post(BLE_EV_CONNECTION);
}

static void disconnected(struct bt_conn *c, uint8_t reason)
{
	if (c != conn) {
		return;
	}
	LOG_INF("phone disconnected (0x%02x)", reason);
	bt_conn_unref(conn);
	conn = NULL;
	notify_on = false;
	companion_connected(false);
	if (atomic_set(&passkey, -1) != -1) {
		post(BLE_EV_PAIR_FAILED);
	}
	post(BLE_EV_CONNECTION);
}

static void recycled(void)
{
	k_work_submit(&adv_work);
}

static void security_changed(struct bt_conn *c, bt_security_t level, enum bt_security_err err)
{
	ARG_UNUSED(c);
	if (err) {
		LOG_WRN("security failed (%d)", err);
	} else {
		LOG_INF("link secured (level %d)", level);
	}
}

BT_CONN_CB_DEFINE(conn_cb) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
	.security_changed = security_changed,
};

static void passkey_display(struct bt_conn *c, unsigned int key)
{
	ARG_UNUSED(c);
	atomic_set(&passkey, key);
	LOG_INF("pairing: passkey shown");
	post(BLE_EV_PASSKEY);
}

static void auth_cancel(struct bt_conn *c)
{
	ARG_UNUSED(c);
	if (atomic_set(&passkey, -1) != -1) {
		post(BLE_EV_PAIR_FAILED);
	}
}

static void pairing_complete(struct bt_conn *c, bool bonded)
{
	ARG_UNUSED(c);
	LOG_INF("paired%s", bonded ? " and bonded" : "");
	atomic_set(&passkey, -1);
	post(BLE_EV_PAIRED);
}

static void pairing_failed(struct bt_conn *c, enum bt_security_err reason)
{
	ARG_UNUSED(c);
	LOG_WRN("pairing failed (%d)", reason);
	atomic_set(&passkey, -1);
	post(BLE_EV_PAIR_FAILED);
}

/* Display only: the phone's user types the passkey we show. */
static struct bt_conn_auth_cb auth_cb = {
	.passkey_display = passkey_display,
	.cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
};

static int start(void)
{
	int ret;

	if (started) {
		return 0;
	}
	ret = bt_enable(NULL);
	if (ret != 0) {
		LOG_ERR("Bluetooth not available (%d)", ret);
		return ret;
	}
	settings_load_subtree("bt");
	bt_conn_auth_cb_register(&auth_cb);
	bt_conn_auth_info_cb_register(&auth_info_cb);
	companion_attach(wake);
	started = true;
	return 0;
}

void ble_init(void)
{
	settings_load_subtree("ble");
	available = mesh_protocol() == MESH_PROTO_MESHCORE;
	if (available && enabled && start() == 0) {
		k_work_submit(&adv_work);
	}
}

bool ble_available(void)
{
	return available;
}

void ble_set_enabled(bool on)
{
	enabled = on;
	settings_save_one("ble/on", &enabled, sizeof(enabled));
	if (!available) {
		return;
	}
	if (on) {
		if (start() == 0) {
			k_work_submit(&adv_work);
		}
		return;
	}
	if (started) {
		bt_le_adv_stop();
		if (conn) {
			bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		}
	}
}

bool ble_connected(void)
{
	return conn && notify_on;
}

int ble_passkey(void)
{
	return atomic_get(&passkey);
}

void ble_pairing_cancel(void)
{
	if (conn && atomic_get(&passkey) != -1) {
		bt_conn_auth_cancel(conn);
	}
}

void ble_forget(void)
{
	if (!started) {
		return;
	}
	if (conn) {
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
	bt_unpair(BT_ID_DEFAULT, NULL);
	LOG_INF("paired phones forgotten");
}

#else /* no Bluetooth (emulator) */

void ble_init(void)
{
	settings_load_subtree("ble");
}

bool ble_available(void)
{
	return false;
}

void ble_set_enabled(bool on)
{
	enabled = on;
	settings_save_one("ble/on", &enabled, sizeof(enabled));
}

bool ble_connected(void)
{
	return false;
}

int ble_passkey(void)
{
	return -1;
}

void ble_pairing_cancel(void)
{
}

void ble_forget(void)
{
}

#endif
