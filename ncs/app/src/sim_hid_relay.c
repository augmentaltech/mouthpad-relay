/*
 * SFP-657 Option B: translate the iOS sim's custom 8-byte HID reports (notify
 * char 6E40FF02) into the relay's HOGP peripheral reports, so the OS sees the
 * sim as a mouse/keyboard exactly like a real MouthPad.
 *
 * UNTESTED ON HARDWARE (phone unavailable at implementation time). The mouse
 * path is straightforward; the keyboard path needs a proto-Keycode -> HID-usage
 * map (the companion's key_injection has it) — here we pass the low byte through
 * as a best-effort placeholder, marked TODO.
 */
#define LOG_MODULE_NAME sim_hid_relay
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>

#include "sim_hid_relay.h"

/* Relay HOGP report IDs / lengths (mirror ble_hids.c). */
#define REPORT_ID_BUTTONS   1
#define REPORT_ID_MOVEMENT  2
#define REPORT_ID_KEYBOARD  4
#define LEN_BUTTONS         3   /* buttons, wheel, AC pan */
#define LEN_MOVEMENT        3   /* 12-bit X + 12-bit Y packed */
#define LEN_KEYBOARD        8   /* modifiers, reserved, 6 keys */

/* Sim report layout. */
#define SIM_HID_REPORT_LEN  8
#define SIM_HID_MOUSE       0
#define SIM_HID_KEY         1

/* 6E40FF02-B5A3-F393-E0A9-E50E24DCCA9E */
static const struct bt_uuid_128 sim_hid_char_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0x6e40ff02, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e));

extern int ble_hids_send_report(uint8_t report_id, const uint8_t *data, uint16_t len);
/* USB HID submit: data is [report_id, payload...]. Mirrors the MouthPad path in
 * ble_hid.c so the sim drives the relay's USB mouse/keyboard, not just BLE. */
extern int usb_hid_send_report(const uint8_t *data, uint16_t len);

static struct bt_gatt_subscribe_params sub_params;
static struct bt_gatt_discover_params disc_params;
static bool subscribed;

static void forward_mouse(const uint8_t *r)
{
	int16_t dx = (int16_t)sys_get_le16(&r[1]);
	int16_t dy = (int16_t)sys_get_le16(&r[3]);
	uint8_t buttons = r[5];
	int8_t wheel = (int8_t)r[6];
	int8_t pan = (int8_t)r[7];

	/* Buttons report (ID 1): buttons, wheel, AC pan. */
	uint8_t btn[LEN_BUTTONS] = { buttons, (uint8_t)wheel, (uint8_t)pan };
	(void)ble_hids_send_report(REPORT_ID_BUTTONS, btn, LEN_BUTTONS);
	uint8_t btn_usb[1 + LEN_BUTTONS] = { REPORT_ID_BUTTONS, btn[0], btn[1], btn[2] };
	(void)usb_hid_send_report(btn_usb, sizeof(btn_usb));

	/* Movement report (ID 2): 12-bit X then 12-bit Y, little-endian nibble pack. */
	int16_t x = CLAMP(dx, -2048, 2047);
	int16_t y = CLAMP(dy, -2048, 2047);
	uint16_t ux = (uint16_t)x & 0x0FFF;
	uint16_t uy = (uint16_t)y & 0x0FFF;
	uint8_t mov[LEN_MOVEMENT];
	mov[0] = (uint8_t)(ux & 0xFF);
	mov[1] = (uint8_t)((ux >> 8) & 0x0F) | (uint8_t)((uy & 0x0F) << 4);
	mov[2] = (uint8_t)((uy >> 4) & 0xFF);
	(void)ble_hids_send_report(REPORT_ID_MOVEMENT, mov, LEN_MOVEMENT);
	uint8_t mov_usb[1 + LEN_MOVEMENT] = { REPORT_ID_MOVEMENT, mov[0], mov[1], mov[2] };
	(void)usb_hid_send_report(mov_usb, sizeof(mov_usb));
}

static void forward_key(const uint8_t *r)
{
	uint16_t keycode = sys_get_le16(&r[1]);   /* proto Keycode */
	uint8_t modifiers = r[3];
	uint8_t pressed = r[4];

	/* Keyboard report (ID 4): modifiers, reserved, key1..key6.
	 * TODO: proto Keycode -> HID usage mapping (companion key_injection has it).
	 * Placeholder: pass the low byte through as the usage. */
	uint8_t kbd[LEN_KEYBOARD] = {0};
	kbd[0] = modifiers;
	kbd[2] = pressed ? (uint8_t)(keycode & 0xFF) : 0;
	(void)ble_hids_send_report(REPORT_ID_KEYBOARD, kbd, LEN_KEYBOARD);
	uint8_t kbd_usb[1 + LEN_KEYBOARD] = { REPORT_ID_KEYBOARD };
	memcpy(&kbd_usb[1], kbd, LEN_KEYBOARD);
	(void)usb_hid_send_report(kbd_usb, sizeof(kbd_usb));
}

static uint8_t notify_cb(struct bt_conn *conn,
			 struct bt_gatt_subscribe_params *params,
			 const void *data, uint16_t length)
{
	if (!data) {
		LOG_INF("sim HID unsubscribed");
		subscribed = false;
		return BT_GATT_ITER_STOP;
	}
	if (length < SIM_HID_REPORT_LEN) {
		LOG_WRN("sim HID report too short (%u)", length);
		return BT_GATT_ITER_CONTINUE;
	}
	const uint8_t *r = data;
	LOG_DBG("sim HID notify len=%u type=%u", length, r[0]);
	if (r[0] == SIM_HID_MOUSE) {
		forward_mouse(r);
	} else if (r[0] == SIM_HID_KEY) {
		forward_key(r);
	}
	return BT_GATT_ITER_CONTINUE;
}

/* Second-stage discovery: locate the CCC descriptor that follows the HID value
 * characteristic, then subscribe. Discovering the CCC explicitly (rather than
 * assuming value_handle+1) is robust to the iOS GATT layout. */
static uint8_t ccc_discover_cb(struct bt_conn *conn,
			       const struct bt_gatt_attr *attr,
			       struct bt_gatt_discover_params *params)
{
	if (!attr) {
		LOG_ERR("sim HID CCC descriptor not found (val %u)",
			sub_params.value_handle);
		return BT_GATT_ITER_STOP;
	}

	sub_params.notify = notify_cb;
	sub_params.value = BT_GATT_CCC_NOTIFY;
	sub_params.ccc_handle = attr->handle;

	int err = bt_gatt_subscribe(conn, &sub_params);
	if (err && err != -EALREADY) {
		LOG_ERR("sim HID subscribe failed (err %d)", err);
	} else {
		subscribed = true;
		LOG_INF("sim HID subscribed (val %u ccc %u) — normalizing to HOGP",
			sub_params.value_handle, sub_params.ccc_handle);
	}
	return BT_GATT_ITER_STOP;
}

static uint8_t discover_cb(struct bt_conn *conn,
			   const struct bt_gatt_attr *attr,
			   struct bt_gatt_discover_params *params)
{
	if (!attr) {
		LOG_WRN("sim HID char (6E40FF02) not found");
		return BT_GATT_ITER_STOP;
	}

	/* attr is the characteristic declaration; the value handle is +1. Now
	 * discover the CCC descriptor in the range after the value handle. */
	sub_params.value_handle = bt_gatt_attr_value_handle(attr);
	LOG_INF("sim HID char found: decl %u val %u — discovering CCC",
		attr->handle, sub_params.value_handle);

	static struct bt_uuid_16 ccc_uuid;
	ccc_uuid = (struct bt_uuid_16)BT_UUID_INIT_16(BT_UUID_GATT_CCC_VAL);
	disc_params.uuid = &ccc_uuid.uuid;
	disc_params.func = ccc_discover_cb;
	disc_params.start_handle = sub_params.value_handle + 1;
	disc_params.end_handle = 0xffff;
	disc_params.type = BT_GATT_DISCOVER_DESCRIPTOR;

	int err = bt_gatt_discover(conn, &disc_params);
	if (err) {
		LOG_ERR("sim HID CCC discover failed (err %d)", err);
	}
	return BT_GATT_ITER_STOP;
}

void sim_hid_relay_start(struct bt_conn *conn)
{
	if (subscribed) {
		return;
	}
	disc_params.uuid = &sim_hid_char_uuid.uuid;
	disc_params.func = discover_cb;
	disc_params.start_handle = 0x0001;
	disc_params.end_handle = 0xffff;
	disc_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

	int err = bt_gatt_discover(conn, &disc_params);
	if (err) {
		LOG_ERR("sim HID discover failed (err %d)", err);
	} else {
		LOG_INF("sim HID: discovering 6E40FF02 for HOGP normalization");
	}
}

void sim_hid_relay_stop(void)
{
	subscribed = false;
	memset(&sub_params, 0, sizeof(sub_params));
}
