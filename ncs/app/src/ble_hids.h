/*
 * Copyright (c) 2026 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLE_HIDS_H
#define BLE_HIDS_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @file ble_hids.h
 * @brief BLE peripheral HID output — re-expose the MouthPad's HID over our own
 *        BLE link so a host (phone/PC) sees this device as an HID mouse.
 *
 * This is the BLE counterpart of usb_hid.c: the same MouthPad HID reports that
 * are forwarded to USB are also notified out a BLE HID Service (HOGP). It makes
 * the relay dual-role — central to the MouthPad, peripheral (HID) to a host.
 */

/** @brief Initialize the BLE HID Service and start connectable advertising.
 *  Must be called after the BLE stack is enabled (after ble_transport_init()).
 *  @return 0 on success, negative errno otherwise.
 */
int ble_hids_init(void);

/** @brief Forward one MouthPad HID input report to the connected host.
 *
 * @param report_id  HID report ID (1=buttons, 2=movement, 3=consumer, 4=keyboard).
 * @param data       Report payload WITHOUT the report-ID byte (as delivered by HOGP).
 * @param len        Payload length.
 * @return 0 on success; -ENOTCONN if no host is connected/subscribed; other negative errno.
 */
int ble_hids_send_report(uint8_t report_id, const uint8_t *data, uint16_t len);

/** @brief True while a host is connected to our HID peripheral. */
bool ble_hids_host_connected(void);

#endif /* BLE_HIDS_H */
