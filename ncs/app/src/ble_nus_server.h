/*
 * Copyright (c) 2026 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLE_NUS_SERVER_H
#define BLE_NUS_SERVER_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @file ble_nus_server.h
 * @brief Nordic UART Service *server* on the relay's BLE peripheral.
 *
 * Carries the MouthpadRelay envelope proto over BLE (the same protocol the relay
 * speaks over USB CDC), so a BLE host can stream MouthPad data and send relay
 * commands. Unlike the CDC byte stream, each NUS write/notification is a discrete
 * packet, so no magic/length/CRC framing is used — the payload is a raw encoded
 * AppToRelayMessage (RX) or RelayToAppMessage (TX).
 *
 * RX is queued and drained from the main loop (ble_nus_server_poll_rx) so the
 * relay command handler never runs on the BT RX thread.
 */

/** @brief Initialize the NUS server (registers the GATT service + RX callback). */
int ble_nus_server_init(void);

/** @brief Notify the connected host with raw bytes (an encoded RelayToAppMessage).
 *  @return 0 on success; -ENOTCONN if no host/subscription; other negative errno. */
int ble_nus_server_send(const uint8_t *data, uint16_t len);

/** @brief True while a host is connected to the NUS server. */
bool ble_nus_server_host_connected(void);

/** @brief Pop one queued RX payload (raw AppToRelayMessage from the host) into
 *  @p out. Returns the byte count, or 0 if the queue is empty. Call from the
 *  main loop. */
uint16_t ble_nus_server_poll_rx(uint8_t *out, uint16_t max_len);

#endif /* BLE_NUS_SERVER_H */
