/*
 * Copyright (c) 2026 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * BLE peripheral HID output. Re-exposes the MouthPad's HID (received over HOGP
 * by ble_hid.c) out our own BLE HID Service so a host sees this device as an HID
 * mouse — the BLE counterpart of usb_hid.c. Makes the relay dual-role: central
 * to the MouthPad, peripheral (HID) to the host.
 *
 * The central (MouthPad) link is owned by ble_central.c / ble_hid.c; this module
 * owns the peripheral (host) link. Both register independent BT_CONN_CB_DEFINE
 * callback sets; each ignores connections of the other role (ble_central.c has a
 * role guard, and the callbacks here act only on peripheral-role connections).
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <bluetooth/services/hids.h>
#include <zephyr/logging/log.h>

#include "ble_hids.h"
#include "ble_transport.h"  /* SFP-667: gate central scan on host presence */

LOG_MODULE_REGISTER(ble_hids, LOG_LEVEL_INF);

/* Report IDs / sizes — must match the MouthPad HID report map (usb_hid.c). */
#define REPORT_ID_BUTTONS   1
#define REPORT_ID_MOVEMENT  2
#define REPORT_ID_CONSUMER  3
#define REPORT_ID_KEYBOARD  4

#define INPUT_REP_BUTTONS_LEN   3  /* buttons + wheel + AC pan */
#define INPUT_REP_MOVEMENT_LEN  3  /* 12-bit X + 12-bit Y */
#define INPUT_REP_CONSUMER_LEN  2  /* 16-bit usage */
#define INPUT_REP_KEYBOARD_LEN  8  /* modifiers + reserved + 6 keys */

#define INPUT_REP_BUTTONS_IDX   0
#define INPUT_REP_MOVEMENT_IDX  1
#define INPUT_REP_CONSUMER_IDX  2
#define INPUT_REP_KEYBOARD_IDX  3

#define BASE_USB_HID_SPEC_VERSION 0x0101

BT_HIDS_DEF(hids_obj,
	    INPUT_REP_BUTTONS_LEN,
	    INPUT_REP_MOVEMENT_LEN,
	    INPUT_REP_CONSUMER_LEN,
	    INPUT_REP_KEYBOARD_LEN);

static struct bt_conn *m_host_conn;  /* the peripheral (host) connection, if any */

/*
 * HID report map — byte-identical to usb_hid.c's hid_report_desc so the host
 * sees exactly the reports the MouthPad emits:
 *   Report ID 1: mouse buttons (5) + wheel + AC pan
 *   Report ID 2: mouse movement (12-bit X / 12-bit Y)
 *   Report ID 3: consumer control (16-bit usage)
 *   Report ID 4: keyboard (modifiers + 6 keys)
 */
static const uint8_t report_map[] = {
	0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x95,
	0x05, 0x75, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01,
	0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x01, 0x75, 0x08, 0x95, 0x01, 0x05,
	0x01, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x81, 0x06, 0x05, 0x0C, 0x0A, 0x38,
	0x02, 0x95, 0x01, 0x81, 0x06, 0xC0, 0x85, 0x02, 0x09, 0x01, 0xA1, 0x00, 0x75,
	0x0C, 0x95, 0x02, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0xF8, 0x26,
	0xFF, 0x07, 0x81, 0x06, 0xC0, 0x85, 0x03, 0x05, 0x0C, 0x19, 0x00, 0x2A, 0x3C,
	0x02, 0x15, 0x00, 0x26, 0x3C, 0x02, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00, 0xC0,
	0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x04, 0x05, 0x07, 0x19, 0xE0, 0x29,
	0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
	0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05,
	0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0
};

/* Advertising: HID appearance + HIDS/BAS UUIDs; name in the scan response. */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE,
		      (CONFIG_BT_DEVICE_APPEARANCE >> 0) & 0xff,
		      (CONFIG_BT_DEVICE_APPEARANCE >> 8) & 0xff),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL,
		      BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void advertising_start(void)
{
	/* Connectable, fast advertising. Concurrent with the central scan — the
	 * controller schedules both. Restarted on host disconnect. */
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err == -EALREADY) {
		return;
	}
	if (err) {
		LOG_ERR("HID advertising start failed (err %d)", err);
		return;
	}
	LOG_INF("HID advertising started (host can connect)");
}

/* ── Peripheral (host) connection callbacks — act only on peripheral role ──── */
static void hids_connected(struct bt_conn *conn, uint8_t err)
{
	struct bt_conn_info info;

	if (bt_conn_get_info(conn, &info) != 0 || info.role != BT_CONN_ROLE_PERIPHERAL) {
		return;  /* central (MouthPad) link — handled by ble_central.c */
	}
	if (err) {
		LOG_ERR("HID host connection failed (0x%02x)", err);
		advertising_start();
		return;
	}

	m_host_conn = bt_conn_ref(conn);
	LOG_INF("HID host connected");

	int e = bt_hids_connected(&hids_obj, conn);
	if (e) {
		LOG_ERR("bt_hids_connected failed (err %d)", e);
	}

	/* SFP-667: a host is now attached — let central scanning for MouthPads begin. */
	ble_transport_ble_host_changed(true);

	/* HID notifications require an encrypted link; request it (the host
	 * normally drives pairing, this nudges it / re-encrypts a known bond). */
	e = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (e) {
		LOG_WRN("bt_conn_set_security failed (err %d)", e);
	}

	/* SFP-667: re-arm advertising so a SECOND host can connect (BT_MAX_CONN=8).
	 * The relay serves both the OS HID stack and the companion app (NUS) at once;
	 * without this the advertiser stops on the first connect and the companion can
	 * never find the relay once the OS HID host has grabbed it. */
	advertising_start();
}

static void hids_disconnected(struct bt_conn *conn, uint8_t reason)
{
	if (conn != m_host_conn) {
		return;  /* not our peripheral link */
	}
	LOG_INF("HID host disconnected (0x%02x)", reason);

	(void)bt_hids_disconnected(&hids_obj, conn);
	bt_conn_unref(m_host_conn);
	m_host_conn = NULL;

	/* SFP-667: host gone — stop central scanning and disconnect any MouthPad
	 * (handled in ble_transport on the 1->0 host edge). */
	ble_transport_ble_host_changed(false);

	advertising_start();
}

BT_CONN_CB_DEFINE(hids_conn_callbacks) = {
	.connected = hids_connected,
	.disconnected = hids_disconnected,
};

/* ── Public API ────────────────────────────────────────────────────────────── */
int ble_hids_init(void)
{
	struct bt_hids_init_param params = { 0 };
	struct bt_hids_inp_rep *rep;
	int err;

	params.rep_map.data = report_map;
	params.rep_map.size = sizeof(report_map);

	params.info.bcd_hid = BASE_USB_HID_SPEC_VERSION;
	params.info.b_country_code = 0x00;
	params.info.flags = (BT_HIDS_REMOTE_WAKE | BT_HIDS_NORMALLY_CONNECTABLE);

	rep = &params.inp_rep_group_init.reports[INPUT_REP_BUTTONS_IDX];
	rep->size = INPUT_REP_BUTTONS_LEN;
	rep->id = REPORT_ID_BUTTONS;
	params.inp_rep_group_init.cnt++;

	rep = &params.inp_rep_group_init.reports[INPUT_REP_MOVEMENT_IDX];
	rep->size = INPUT_REP_MOVEMENT_LEN;
	rep->id = REPORT_ID_MOVEMENT;
	params.inp_rep_group_init.cnt++;

	rep = &params.inp_rep_group_init.reports[INPUT_REP_CONSUMER_IDX];
	rep->size = INPUT_REP_CONSUMER_LEN;
	rep->id = REPORT_ID_CONSUMER;
	params.inp_rep_group_init.cnt++;

	rep = &params.inp_rep_group_init.reports[INPUT_REP_KEYBOARD_IDX];
	rep->size = INPUT_REP_KEYBOARD_LEN;
	rep->id = REPORT_ID_KEYBOARD;
	params.inp_rep_group_init.cnt++;

	params.is_mouse = true;

	err = bt_hids_init(&hids_obj, &params);
	if (err) {
		LOG_ERR("bt_hids_init failed (err %d)", err);
		return err;
	}
	LOG_INF("BLE HID Service initialized (%u input reports)", params.inp_rep_group_init.cnt);

	advertising_start();
	return 0;
}

int ble_hids_send_report(uint8_t report_id, const uint8_t *data, uint16_t len)
{
	uint8_t idx;

	if (!m_host_conn) {
		return -ENOTCONN;
	}

	switch (report_id) {
	case REPORT_ID_BUTTONS:  idx = INPUT_REP_BUTTONS_IDX;  break;
	case REPORT_ID_MOVEMENT: idx = INPUT_REP_MOVEMENT_IDX; break;
	case REPORT_ID_CONSUMER: idx = INPUT_REP_CONSUMER_IDX; break;
	case REPORT_ID_KEYBOARD: idx = INPUT_REP_KEYBOARD_IDX; break;
	default:
		return -EINVAL;
	}

	/* Returns -ENOMEM until the host enables this report's CCCD or when the BLE
	 * TX buffers are momentarily exhausted under fast motion; callers treat that
	 * as benign (the report is simply dropped). */
	return bt_hids_inp_rep_send(&hids_obj, m_host_conn, idx, data, len, NULL);
}

bool ble_hids_host_connected(void)
{
	return m_host_conn != NULL;
}
