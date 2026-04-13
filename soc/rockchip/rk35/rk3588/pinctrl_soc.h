/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SOC_ROCKCHIP_RK3588_PINCTRL_SOC_H_
#define ZEPHYR_SOC_ROCKCHIP_RK3588_PINCTRL_SOC_H_

#include <stdint.h>

#include <zephyr/dt-bindings/pinctrl/rockchip-rk3588-pinctrl.h>

/* Encoded as RK_PINMUX(bank, pin, func) in devicetree pinmux cells. */
#define RK3588_PINMUX_BANK(v) (((v) >> 16) & 0xffffU)
#define RK3588_PINMUX_PIN(v)  (((v) >> 8) & 0xffU)
#define RK3588_PINMUX_FUNC(v) ((v) & 0xffU)

enum rk3588_pin_bias {
	RK3588_PIN_BIAS_NONE = 0,
	RK3588_PIN_BIAS_PULL_UP,
	RK3588_PIN_BIAS_PULL_DOWN,
};

struct rk3588_pinctrl_soc_pin {
	uint32_t pinmux;
	uint8_t bias;
	uint8_t drive_strength;
	uint8_t input_enable;
};

typedef struct rk3588_pinctrl_soc_pin pinctrl_soc_pin_t;

#define RK3588_PIN_BIAS_INIT(node_id)                                                       \
	COND_CODE_1(DT_PROP(node_id, bias_pull_up), (RK3588_PIN_BIAS_PULL_UP),               \
		(COND_CODE_1(DT_PROP(node_id, bias_pull_down), (RK3588_PIN_BIAS_PULL_DOWN),     \
				    (RK3588_PIN_BIAS_NONE))))

#define Z_PINCTRL_STATE_PIN_INIT(node_id, prop, idx)                                        \
	{                                                                                      \
		.pinmux = DT_PROP_BY_IDX(node_id, prop, idx),                                   \
		.bias = RK3588_PIN_BIAS_INIT(node_id),                                           \
		.drive_strength = DT_PROP_OR(node_id, drive_strength, 0),                        \
		.input_enable = DT_PROP(node_id, input_enable),                                  \
	},

#define Z_PINCTRL_STATE_PINS_INIT(node_id, prop)                                             \
	{DT_FOREACH_CHILD_VARGS(DT_PHANDLE(node_id, prop),                                    \
				DT_FOREACH_PROP_ELEM, pinmux, Z_PINCTRL_STATE_PIN_INIT)}

#endif /* ZEPHYR_SOC_ROCKCHIP_RK3588_PINCTRL_SOC_H_ */
