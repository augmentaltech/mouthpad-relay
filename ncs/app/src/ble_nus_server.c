/*
 * Copyright (c) 2026 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NUS server on the relay's BLE peripheral — see ble_nus_server.h. RX payloads
 * are queued and drained from the main loop so the relay command handler never
 * runs on the BT RX thread.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <bluetooth/services/nus.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "ble_nus_server.h"

LOG_MODULE_REGISTER(ble_nus_server, LOG_LEVEL_INF);

#define NUS_RX_MSG_MAX     256
#define NUS_RX_QUEUE_DEPTH 8

struct nus_rx_msg {
	uint16_t len;
	uint8_t data[NUS_RX_MSG_MAX];
};

K_MSGQ_DEFINE(nus_rx_q, sizeof(struct nus_rx_msg), NUS_RX_QUEUE_DEPTH, 4);

/* The peripheral (host) connection, ref'd while connected. */
static struct bt_conn *m_host_conn;

static void nus_received(struct bt_conn *conn, const uint8_t *const data, uint16_t len)
{
	struct nus_rx_msg msg;

	if (len > NUS_RX_MSG_MAX) {
		LOG_WRN("NUS RX %u bytes > max %u, dropping", len, NUS_RX_MSG_MAX);
		return;
	}
	msg.len = len;
	memcpy(msg.data, data, len);
	if (k_msgq_put(&nus_rx_q, &msg, K_NO_WAIT) != 0) {
		LOG_WRN("NUS RX queue full, dropping %u bytes", len);
	}
}

static struct bt_nus_cb nus_cb = {
	.received = nus_received,
};

/* Track the peripheral host connection for bt_nus_send(). Acts only on the
 * peripheral role; the central (MouthPad) link is owned elsewhere. */
static void nus_srv_connected(struct bt_conn *conn, uint8_t err)
{
	struct bt_conn_info info;

	if (err || bt_conn_get_info(conn, &info) != 0 ||
	    info.role != BT_CONN_ROLE_PERIPHERAL) {
		return;
	}
	m_host_conn = bt_conn_ref(conn);
}

static void nus_srv_disconnected(struct bt_conn *conn, uint8_t reason)
{
	if (conn == m_host_conn) {
		bt_conn_unref(m_host_conn);
		m_host_conn = NULL;
	}
}

BT_CONN_CB_DEFINE(nus_srv_conn_cb) = {
	.connected = nus_srv_connected,
	.disconnected = nus_srv_disconnected,
};

int ble_nus_server_init(void)
{
	int err = bt_nus_init(&nus_cb);

	if (err) {
		LOG_ERR("bt_nus_init failed (err %d)", err);
		return err;
	}
	LOG_INF("NUS server initialized (relay proto over BLE)");
	return 0;
}

int ble_nus_server_send(const uint8_t *data, uint16_t len)
{
	if (!m_host_conn) {
		return -ENOTCONN;
	}
	return bt_nus_send(m_host_conn, data, len);
}

bool ble_nus_server_host_connected(void)
{
	return m_host_conn != NULL;
}

uint16_t ble_nus_server_poll_rx(uint8_t *out, uint16_t max_len)
{
	struct nus_rx_msg msg;

	if (k_msgq_get(&nus_rx_q, &msg, K_NO_WAIT) != 0) {
		return 0;
	}
	uint16_t n = (msg.len < max_len) ? msg.len : max_len;
	memcpy(out, msg.data, n);
	return n;
}
