/*
 * SFP-657 Option B: normalize the iOS sim's custom HID characteristic into the
 * relay's own HOGP peripheral, so the OS sees a mouse/keyboard for the sim just
 * like it does for a real MouthPad.
 *
 * The sim has no HOGP; it emits 8-byte custom reports on a notify characteristic
 * (6E40FF02, inside the NUS service). We discover + subscribe to it on a sim
 * link and translate each report into ble_hids_send_report() calls.
 *
 * Sim report (little-endian, 8 bytes), per mouthpad-sim CFirmware sim_hid.h:
 *   [0] type: 0=MOUSE, 1=KEY
 *   mouse: [1:2] int16 dx, [3:4] int16 dy, [5] buttons(bit0 L,1 R,2 M),
 *          [6] int8 wheel, [7] int8 pan
 *   key:   [1:2] uint16 keycode (proto Keycode), [3] modifiers, [4] pressed
 */
#ifndef SIM_HID_RELAY_H
#define SIM_HID_RELAY_H

#include <zephyr/bluetooth/conn.h>

/* Discover + subscribe to the sim HID characteristic on this connection.
 * Call once a sim link is established (after GATT/NUS discovery). */
void sim_hid_relay_start(struct bt_conn *conn);

/* Stop forwarding (unsubscribe). Call on disconnect. */
void sim_hid_relay_stop(void);

#endif /* SIM_HID_RELAY_H */
