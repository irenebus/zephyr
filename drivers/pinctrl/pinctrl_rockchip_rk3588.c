/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <pinctrl_soc.h>
#include <zephyr/sys/sys_io.h>

LOG_MODULE_REGISTER(pinctrl_rockchip_rk3588, CONFIG_PINCTRL_LOG_LEVEL);

#define DT_DRV_COMPAT rockchip_rk3588_pinctrl

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
	     "Unsupported number of instances");

/* Current board path only uses GPIO1B (pins 8..15) on BUS IOC. */
#define RK3588_GPIO1_BANK                    1U
#define RK3588_GPIO1B_PIN_FIRST              8U
#define RK3588_GPIO1B_PIN_LAST               15U
#define RK3588_GPIO1B_IOMUX_SEL_L_OFFSET     0x0028U
#define RK3588_GPIO1B_IOMUX_SEL_H_OFFSET     0x002cU

#define RK3588_ELEC_REG_BLOCK_OFFSET         0x1000U
#define RK3588_GPIO1B_DS_L_OFFSET            0x0028U
#define RK3588_GPIO1B_DS_H_OFFSET            0x002cU
#define RK3588_GPIO1B_P_OFFSET               0x0114U
#define RK3588_GPIO1B_IE_OFFSET              0x0184U

#define RK3588_IOMUX_FIELD_BITS              4U
#define RK3588_DS_FIELD_BITS                 4U
#define RK3588_BIAS_FIELD_BITS               2U
#define RK3588_IE_FIELD_BITS                 1U

#define RK3588_PINS_PER_HALF                 4U
#define RK3588_IOMUX_MAX_FUNC                0x0fU

static const mm_reg_t bus_ioc_base = DT_REG_ADDR(DT_INST_PHANDLE(0, rockchip_grf));
K_SEM_DEFINE(rk3588_pinctrl_lock, 1, 1);

static int rk3588_program_iomux(const pinctrl_soc_pin_t *pin)
{
	uint32_t bank = RK3588_PINMUX_BANK(pin->pinmux);
	uint32_t pin_id = RK3588_PINMUX_PIN(pin->pinmux);
	uint32_t func = RK3588_PINMUX_FUNC(pin->pinmux);
	uint32_t mask;
	uint32_t val;
	uint32_t shift;
	uintptr_t addr;

	if (bank != RK3588_GPIO1_BANK) {
		return -ENOTSUP;
	}

	if ((pin_id < RK3588_GPIO1B_PIN_FIRST) || (pin_id > RK3588_GPIO1B_PIN_LAST)) {
		return -ENOTSUP;
	}

	shift = ((pin_id - RK3588_GPIO1B_PIN_FIRST) % RK3588_PINS_PER_HALF) *
		RK3588_IOMUX_FIELD_BITS;
	addr = bus_ioc_base +
		((pin_id < (RK3588_GPIO1B_PIN_FIRST + RK3588_PINS_PER_HALF)) ?
		 RK3588_GPIO1B_IOMUX_SEL_L_OFFSET : RK3588_GPIO1B_IOMUX_SEL_H_OFFSET);

	mask = RK3588_IOMUX_MAX_FUNC << shift;
	val = (mask << 16) | ((func & RK3588_IOMUX_MAX_FUNC) << shift);

	/*
	 * RK3588 IOC uses high-word write mask semantics.
	 * Upper 16 bits select updated fields, lower 16 bits carry new value.
	 */
	sys_write32(val, addr);

	return 0;
}

static int rk3588_program_drive_strength(const pinctrl_soc_pin_t *pin)
{
	uint32_t bank = RK3588_PINMUX_BANK(pin->pinmux);
	uint32_t pin_id = RK3588_PINMUX_PIN(pin->pinmux);
	uint32_t ds = pin->drive_strength;
	uint32_t shift;
	uintptr_t addr;
	uint32_t mask;
	uint32_t val;

	if (ds == 0U) {
		return 0;
	}

	if (bank != RK3588_GPIO1_BANK) {
		return -ENOTSUP;
	}

	if ((pin_id < RK3588_GPIO1B_PIN_FIRST) || (pin_id > RK3588_GPIO1B_PIN_LAST)) {
		return -ENOTSUP;
	}

	/* Supported drive-strength encoding values: [0, 4, 2, 6, 1, 5]. */
	if (!((ds == 1U) || (ds == 2U) || (ds == 4U) || (ds == 5U) || (ds == 6U))) {
		return -EINVAL;
	}

	shift = ((pin_id - RK3588_GPIO1B_PIN_FIRST) % RK3588_PINS_PER_HALF) *
		RK3588_DS_FIELD_BITS;
	addr = bus_ioc_base + RK3588_ELEC_REG_BLOCK_OFFSET +
		((pin_id < (RK3588_GPIO1B_PIN_FIRST + RK3588_PINS_PER_HALF)) ?
		 RK3588_GPIO1B_DS_L_OFFSET : RK3588_GPIO1B_DS_H_OFFSET);

	mask = ((1U << RK3588_DS_FIELD_BITS) - 1U) << shift;
	val = (mask << 16) | ((ds & ((1U << RK3588_DS_FIELD_BITS) - 1U)) << shift);

	sys_write32(val, addr);

	return 0;
}

static int rk3588_program_bias(const pinctrl_soc_pin_t *pin)
{
	uint32_t bank = RK3588_PINMUX_BANK(pin->pinmux);
	uint32_t pin_id = RK3588_PINMUX_PIN(pin->pinmux);
	uint32_t bias = pin->bias;
	uint32_t bias_val;
	uint32_t shift;
	uintptr_t addr;
	uint32_t mask;
	uint32_t val;

	if (bias == RK3588_PIN_BIAS_NONE) {
		return 0;
	}

	if (bank != RK3588_GPIO1_BANK) {
		return -ENOTSUP;
	}

	if ((pin_id < RK3588_GPIO1B_PIN_FIRST) || (pin_id > RK3588_GPIO1B_PIN_LAST)) {
		return -ENOTSUP;
	}

	/* b0=0 disables pull, b1=0 selects pull-down: PU=01, PD=11. */
	if (bias == RK3588_PIN_BIAS_PULL_UP) {
		bias_val = 0x1U;
	} else if (bias == RK3588_PIN_BIAS_PULL_DOWN) {
		bias_val = 0x3U;
	} else {
		return -EINVAL;
	}

	shift = (pin_id - RK3588_GPIO1B_PIN_FIRST) * RK3588_BIAS_FIELD_BITS;
	addr = bus_ioc_base + RK3588_ELEC_REG_BLOCK_OFFSET + RK3588_GPIO1B_P_OFFSET;

	mask = ((1U << RK3588_BIAS_FIELD_BITS) - 1U) << shift;
	val = (mask << 16) | ((bias_val & ((1U << RK3588_BIAS_FIELD_BITS) - 1U)) << shift);

	sys_write32(val, addr);

	return 0;
}

static int rk3588_program_input_enable(const pinctrl_soc_pin_t *pin)
{
	uint32_t bank = RK3588_PINMUX_BANK(pin->pinmux);
	uint32_t pin_id = RK3588_PINMUX_PIN(pin->pinmux);
	uint32_t ie = pin->input_enable;
	uint32_t shift;
	uintptr_t addr;
	uint32_t mask;
	uint32_t val;

	if (ie == 0U) {
		return 0;
	}

	if (bank != RK3588_GPIO1_BANK) {
		return -ENOTSUP;
	}

	if ((pin_id < RK3588_GPIO1B_PIN_FIRST) || (pin_id > RK3588_GPIO1B_PIN_LAST)) {
		return -ENOTSUP;
	}

	shift = pin_id - RK3588_GPIO1B_PIN_FIRST;
	addr = bus_ioc_base + RK3588_ELEC_REG_BLOCK_OFFSET + RK3588_GPIO1B_IE_OFFSET;

	mask = ((1U << RK3588_IE_FIELD_BITS) - 1U) << shift;
	val = (mask << 16) | ((ie & 1U) << shift);

	sys_write32(val, addr);

	return 0;
}

static int rk3588_pinctrl_validate(const pinctrl_soc_pin_t *pin)
{
	uint32_t bank = RK3588_PINMUX_BANK(pin->pinmux);
	uint32_t pin_id = RK3588_PINMUX_PIN(pin->pinmux);
	uint32_t func = RK3588_PINMUX_FUNC(pin->pinmux);

	if (bank != RK3588_GPIO1_BANK) {
		return -EINVAL;
	}

	if ((pin_id < RK3588_GPIO1B_PIN_FIRST) || (pin_id > RK3588_GPIO1B_PIN_LAST)) {
		return -EINVAL;
	}

	if (func > RK3588_IOMUX_MAX_FUNC) {
		return -EINVAL;
	}

	if (pin->bias > RK3588_PIN_BIAS_PULL_DOWN) {
		return -EINVAL;
	}

	return 0;
}

int pinctrl_configure_pins(const pinctrl_soc_pin_t *pins, uint8_t pin_cnt, uintptr_t reg)
{
	k_sem_take(&rk3588_pinctrl_lock, K_FOREVER);

	ARG_UNUSED(reg);

	for (uint8_t i = 0U; i < pin_cnt; i++) {
		int ret = rk3588_pinctrl_validate(&pins[i]);

		if (ret < 0) {
			k_sem_give(&rk3588_pinctrl_lock);
			LOG_ERR("invalid pinctrl entry at index %u", i);
			return ret;
		}

		ret = rk3588_program_iomux(&pins[i]);
		if (ret < 0) {
			k_sem_give(&rk3588_pinctrl_lock);
			LOG_ERR("failed to configure IOMUX for pin %u (err %d)", i, ret);
			return ret;
		}

		ret = rk3588_program_drive_strength(&pins[i]);
		if (ret < 0) {
			k_sem_give(&rk3588_pinctrl_lock);
			LOG_ERR("failed to configure drive-strength for pin %u (err %d)", i, ret);
			return ret;
		}

		ret = rk3588_program_bias(&pins[i]);
		if (ret < 0) {
			k_sem_give(&rk3588_pinctrl_lock);
			LOG_ERR("failed to configure bias for pin %u (err %d)", i, ret);
			return ret;
		}

		ret = rk3588_program_input_enable(&pins[i]);
		if (ret < 0) {
			k_sem_give(&rk3588_pinctrl_lock);
			LOG_ERR("failed to configure input-enable for pin %u (err %d)", i, ret);
			return ret;
		}
	}

	k_sem_give(&rk3588_pinctrl_lock);

	return 0;
}
