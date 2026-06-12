/*
 * Copyright (c) 2026 Augmental Tech
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * KTD2026 3-channel I2C RGB LED driver (see led_ktd2026.h). Compiled in always,
 * but inert unless the devicetree has an `augmental,ktd2026` node.
 */

#include "led_ktd2026.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(led_ktd2026, LOG_LEVEL_INF);

#if DT_NODE_EXISTS(DT_NODELABEL(ktd2026))

#include <zephyr/drivers/i2c.h>

/* Register map (KTD2026 datasheet) */
#define KTD2026_REG_EN_RST       0x00U
#define KTD2026_REG_FLASH_PERIOD 0x01U
#define KTD2026_REG_PWM1_TIMER   0x02U
#define KTD2026_REG_CH_CTRL      0x04U
#define KTD2026_REG_RAMP         0x05U
#define KTD2026_REG_LED1_IOUT    0x06U

/* Reg0[2:0] reset commands */
#define KTD2026_RESET_CHIP       0x07U
/* Reg0[4:3] enable: always-on while I2C active */
#define KTD2026_EN_ALWAYS_ON     0x18U

/* 2-bit per-channel control modes (Reg4) */
#define KTD2026_CH_ALWAYS_OFF    0x00U
#define KTD2026_CH_ALWAYS_ON     0x01U

/* Default per-channel current: 0.125mA * (IOUT + 1) */
#define KTD2026_DEFAULT_IOUT     0x01U

/* Bus + pins come from devicetree; the 7-bit address is a compile-time Kconfig
 * (CONFIG_KTD2026_I2C_ADDR, default 0x32 = Dotto, 0x30 = dev kit) so a board with
 * a different part in the KTD2026 family can be selected without editing the DTS.
 * Not const — .addr is overridden from Kconfig in ktd2026_init(). */
static struct i2c_dt_spec ktd = I2C_DT_SPEC_GET(DT_NODELABEL(ktd2026));
static uint8_t channel_ctrl_reg;
static bool ready;

static int write_reg(uint8_t reg, uint8_t value)
{
	return i2c_reg_write_byte_dt(&ktd, reg, value);
}

bool ktd2026_present(void)
{
	return device_is_ready(ktd.bus);
}

int ktd2026_init(void)
{
	if (!device_is_ready(ktd.bus)) {
		LOG_ERR("KTD2026 I2C bus not ready");
		return -ENODEV;
	}

	/* Address from Kconfig (overrides the devicetree reg). */
	ktd.addr = CONFIG_KTD2026_I2C_ADDR;
	LOG_INF("KTD2026 I2C address 0x%02x", ktd.addr);

	/* Reset whole chip. The datasheet says the reset command ends in a NACK that
	 * must be ignored, so don't treat an I2C error here as fatal. */
	uint8_t reset[2] = { KTD2026_REG_EN_RST, KTD2026_RESET_CHIP };
	(void)i2c_write_dt(&ktd, reset, sizeof(reset));
	k_busy_wait(300); /* >=200us after full reset */

	int err = write_reg(KTD2026_REG_EN_RST, KTD2026_EN_ALWAYS_ON);
	if (err) {
		LOG_ERR("KTD2026 enable failed (err %d) — is it powered/wired?", err);
		return err;
	}

	for (uint8_t ch = 0; ch < 3; ch++) {
		(void)write_reg(KTD2026_REG_LED1_IOUT + ch, KTD2026_DEFAULT_IOUT);
	}

	channel_ctrl_reg = 0x00;
	err = write_reg(KTD2026_REG_CH_CTRL, channel_ctrl_reg);
	if (err) {
		return err;
	}

	ready = true;
	LOG_INF("KTD2026 RGB LED driver initialized");
	return 0;
}

static int apply_ctrl(void)
{
	return write_reg(KTD2026_REG_CH_CTRL, channel_ctrl_reg);
}

int ktd2026_channel_on(uint8_t channel)
{
	if (!ready || channel > KTD2026_CH3) {
		return -EINVAL;
	}
	uint8_t shift = channel * 2;
	channel_ctrl_reg &= ~(0x03U << shift);
	channel_ctrl_reg |= (KTD2026_CH_ALWAYS_ON << shift);
	return apply_ctrl();
}

int ktd2026_channel_off(uint8_t channel)
{
	if (!ready || channel > KTD2026_CH3) {
		return -EINVAL;
	}
	channel_ctrl_reg &= ~(0x03U << (channel * 2));
	return apply_ctrl();
}

int ktd2026_all_off(void)
{
	if (!ready) {
		return -EINVAL;
	}
	channel_ctrl_reg = 0x00;
	return apply_ctrl();
}

#else /* no ktd2026 node — inert stubs */

bool ktd2026_present(void) { return false; }
int ktd2026_init(void) { return -ENODEV; }
int ktd2026_channel_on(uint8_t channel) { ARG_UNUSED(channel); return -ENODEV; }
int ktd2026_channel_off(uint8_t channel) { ARG_UNUSED(channel); return -ENODEV; }
int ktd2026_all_off(void) { return -ENODEV; }

#endif /* DT_NODE_EXISTS(ktd2026) */
