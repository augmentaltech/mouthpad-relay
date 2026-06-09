/*
 * Copyright (c) 2026 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal KTD2026 3-channel I2C RGB LED driver (no in-tree Zephyr driver).
 * Ported from mouthpadv1 technolingus/led_ktd2026.c to the Zephyr I2C API.
 * Active only when the devicetree has an `augmental,ktd2026` node (Vox/Dotto).
 * Channels are 0-based: CH0→LED1, CH1→LED2, CH2→LED3 (IOUT regs + 2-bit ctrl).
 */

#ifndef LED_KTD2026_H_
#define LED_KTD2026_H_

#include <stdbool.h>
#include <stdint.h>

#define KTD2026_CH1 0U
#define KTD2026_CH2 1U
#define KTD2026_CH3 2U

/** True if a KTD2026 is present in the devicetree and the I2C bus is ready. */
bool ktd2026_present(void);

/** Reset, enable always-on, default per-channel current, all channels off. */
int ktd2026_init(void);

/** Drive a channel constantly on / off (0-based channel 0..2). */
int ktd2026_channel_on(uint8_t channel);
int ktd2026_channel_off(uint8_t channel);

/** All three channels off. */
int ktd2026_all_off(void);

#endif /* LED_KTD2026_H_ */
