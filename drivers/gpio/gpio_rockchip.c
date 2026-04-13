/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT rockchip_rk3588_gpio

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_utils.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <pinctrl_soc.h>
#include <zephyr/sys/slist.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define RK_GPIO_SWPORT_DR_L      0x0000
#define RK_GPIO_SWPORT_DR_H      0x0004
#define RK_GPIO_SWPORT_DDR_L     0x0008
#define RK_GPIO_SWPORT_DDR_H     0x000c

/* Interrupt registers */
#define RK_GPIO_INT_EN_L         0x0010
#define RK_GPIO_INT_EN_H         0x0014
#define RK_GPIO_INT_MASK_L       0x0018
#define RK_GPIO_INT_MASK_H       0x001c
#define RK_GPIO_INT_TYPE_L       0x0020
#define RK_GPIO_INT_TYPE_H       0x0024
#define RK_GPIO_INT_POLARITY_L   0x0028
#define RK_GPIO_INT_POLARITY_H   0x002c
#define RK_GPIO_INT_BOTHEDGE_L   0x0030
#define RK_GPIO_INT_BOTHEDGE_H   0x0034

/* Debounce and DBCLK */
#define RK_GPIO_DEBOUNCE_L       0x0038
#define RK_GPIO_DEBOUNCE_H       0x003c
#define RK_GPIO_DBCLK_DIV_EN_L   0x0040
#define RK_GPIO_DBCLK_DIV_EN_H   0x0044
#define RK_GPIO_DBCLK_DIV_CON    0x0048

/* Status / EOI / external port */
#define RK_GPIO_INT_STATUS       0x0050
#define RK_GPIO_INT_RAWSTATUS    0x0058
#define RK_GPIO_PORT_EOI_L       0x0060
#define RK_GPIO_PORT_EOI_H       0x0064
#define RK_GPIO_EXT_PORT         0x0070

#define RK_GPIO_VER_ID           0x0078

/* Group control registers */
#define RK_GPIO_GPIO_REG_GROUP_L 0x0100
#define RK_GPIO_GPIO_REG_GROUP_H 0x0104
#define RK_GPIO_GPIO_VIRTUAL_EN  0x0108

struct gpio_rockchip_config {
	struct gpio_driver_config common;
	uintptr_t reg_base;
	uint32_t bank;
	uint32_t irq_num;
	uint32_t irq_prio;
};

struct gpio_rockchip_data {
	struct gpio_driver_data common;
	struct k_spinlock lock;
	sys_slist_t callbacks;
	bool irq_connected;
};

static void gpio_rockchip_isr(const struct device *dev);

#define RK3588_GPIO_FUNC_GPIO           0U
#define RK3588_PINCTRL_BANK1            1U
#define RK3588_PINCTRL_BANK1_PIN_FIRST  8U
#define RK3588_PINCTRL_BANK1_PIN_LAST   15U

#if defined(RK_PINMUX) && defined(RK3588_PIN_BIAS_NONE) && \
	defined(RK3588_PIN_BIAS_PULL_UP) && defined(RK3588_PIN_BIAS_PULL_DOWN)
#define GPIO_ROCKCHIP_HAS_SOC_PINCTRL 1
#endif

static inline bool gpio_rockchip_pin_valid(const struct gpio_rockchip_config *cfg,
					   gpio_pin_t pin)
{
	if (pin >= 32U) {
		return false;
	}

	return (BIT(pin) & cfg->common.port_pin_mask) != 0U;
}

static inline uint32_t gpio_rockchip_read_pair(uintptr_t base, uint32_t reg_l, uint32_t reg_h)
{
	uint32_t low = sys_read32(base + reg_l) & 0xFFFFU;
	uint32_t high = sys_read32(base + reg_h) & 0xFFFFU;

	return low | (high << 16);
}

static inline bool gpio_rockchip_pinctrl_supported(const struct gpio_rockchip_config *cfg,
						   gpio_pin_t pin)
{
	return (cfg->bank == RK3588_PINCTRL_BANK1) &&
		(pin >= RK3588_PINCTRL_BANK1_PIN_FIRST) &&
		(pin <= RK3588_PINCTRL_BANK1_PIN_LAST);
}

#ifdef GPIO_ROCKCHIP_HAS_SOC_PINCTRL
static int gpio_rockchip_apply_pinctrl(const struct gpio_rockchip_config *cfg, gpio_pin_t pin,
					       gpio_flags_t flags)
{
	pinctrl_soc_pin_t pcfg = {
		.pinmux = RK_PINMUX(cfg->bank, pin, RK3588_GPIO_FUNC_GPIO),
		.bias = RK3588_PIN_BIAS_NONE,
		.drive_strength = 0U,
		.input_enable = ((flags & GPIO_INPUT) != 0U) ? 1U : 0U,
	};

	if ((flags & GPIO_PULL_UP) != 0U) {
		pcfg.bias = RK3588_PIN_BIAS_PULL_UP;
	} else if ((flags & GPIO_PULL_DOWN) != 0U) {
		pcfg.bias = RK3588_PIN_BIAS_PULL_DOWN;
	}

	return pinctrl_configure_pins(&pcfg, 1, PINCTRL_REG_NONE);
}
#else
static int gpio_rockchip_apply_pinctrl(const struct gpio_rockchip_config *cfg, gpio_pin_t pin,
					       gpio_flags_t flags)
{
	ARG_UNUSED(cfg);
	ARG_UNUSED(pin);
	ARG_UNUSED(flags);

	return -ENOTSUP;
}
#endif

/*
 * RK3588 GPIO L/H registers use write-enable in [31:16] and data in [15:0].
 * Writes only affect bits set in the high-half write mask.
 */
static inline void gpio_rockchip_write_we_pair(uintptr_t base, uint32_t reg_l, uint32_t reg_h,
						uint32_t mask, uint32_t value)
{
	uint32_t low_mask = mask & 0xFFFFU;
	uint32_t high_mask = (mask >> 16) & 0xFFFFU;

	if (low_mask != 0U) {
		uint32_t low_val = value & 0xFFFFU;

		sys_write32((low_mask << 16) | (low_val & low_mask), base + reg_l);
	}

	if (high_mask != 0U) {
		uint32_t high_val = (value >> 16) & 0xFFFFU;

		sys_write32((high_mask << 16) | (high_val & high_mask), base + reg_h);
	}
}

static int gpio_rockchip_pin_configure(const struct device *dev, gpio_pin_t pin,
				       gpio_flags_t flags)
{
	const struct gpio_rockchip_config *cfg = dev->config;
	struct gpio_rockchip_data *data = dev->data;
	const uint32_t pin_mask = BIT(pin);
	bool pinctrl_supported;
	k_spinlock_key_t key;

	if (!gpio_rockchip_pin_valid(cfg, pin)) {
		return -EINVAL;
	}

	if (flags == GPIO_DISCONNECTED) {
		return -ENOTSUP;
	}

	if ((flags & GPIO_SINGLE_ENDED) != 0U) {
		return -ENOTSUP;
	}

	if ((flags & (GPIO_PULL_UP | GPIO_PULL_DOWN)) == (GPIO_PULL_UP | GPIO_PULL_DOWN)) {
		return -ENOTSUP;
	}

	if ((flags & GPIO_DIR_MASK) == GPIO_DIR_MASK) {
		return -ENOTSUP;
	}

	pinctrl_supported = gpio_rockchip_pinctrl_supported(cfg, pin);
	if ((flags & (GPIO_PULL_UP | GPIO_PULL_DOWN)) != 0U) {
		int pret;

		if (!pinctrl_supported) {
			return -ENOTSUP;
		}

		pret = gpio_rockchip_apply_pinctrl(cfg, pin, flags);
		if (pret < 0) {
			return pret;
		}
	} else if (pinctrl_supported && ((flags & GPIO_INPUT) != 0U)) {
		int pret = gpio_rockchip_apply_pinctrl(cfg, pin, flags);

		if ((pret < 0) && (pret != -ENOTSUP)) {
			return pret;
		}
	}

	key = k_spin_lock(&data->lock);

	if ((flags & GPIO_OUTPUT) != 0U) {
		if ((flags & GPIO_OUTPUT_INIT_HIGH) != 0U) {
			gpio_rockchip_write_we_pair(cfg->reg_base, RK_GPIO_SWPORT_DR_L,
						    RK_GPIO_SWPORT_DR_H,
						    pin_mask, pin_mask);
		} else if ((flags & GPIO_OUTPUT_INIT_LOW) != 0U) {
			gpio_rockchip_write_we_pair(cfg->reg_base, RK_GPIO_SWPORT_DR_L,
						    RK_GPIO_SWPORT_DR_H,
						    pin_mask, 0U);
		}

		gpio_rockchip_write_we_pair(cfg->reg_base, RK_GPIO_SWPORT_DDR_L,
					    RK_GPIO_SWPORT_DDR_H,
					    pin_mask, pin_mask);
	} else if ((flags & GPIO_INPUT) != 0U) {
		gpio_rockchip_write_we_pair(cfg->reg_base, RK_GPIO_SWPORT_DDR_L,
					    RK_GPIO_SWPORT_DDR_H,
					    pin_mask, 0U);
	} else {
		k_spin_unlock(&data->lock, key);
		return -ENOTSUP;
	}

	k_spin_unlock(&data->lock, key);

	return 0;
}

static int gpio_rockchip_port_get_raw(const struct device *dev, gpio_port_value_t *value)
{
	const struct gpio_rockchip_config *cfg = dev->config;

	*value = sys_read32(cfg->reg_base + RK_GPIO_EXT_PORT) & cfg->common.port_pin_mask;
	return 0;
}

static int gpio_rockchip_port_set_masked_raw(const struct device *dev,
					    gpio_port_pins_t mask,
					    gpio_port_value_t value)
{
	const struct gpio_rockchip_config *cfg = dev->config;
	struct gpio_rockchip_data *data = dev->data;
	const uint32_t valid_mask = (uint32_t)mask & cfg->common.port_pin_mask;
	uint32_t next;
	k_spinlock_key_t key;

	if (valid_mask == 0U) {
		return 0;
	}

	key = k_spin_lock(&data->lock);
	next = gpio_rockchip_read_pair(cfg->reg_base, RK_GPIO_SWPORT_DR_L,
				       RK_GPIO_SWPORT_DR_H);
	next = (next & ~valid_mask) | ((uint32_t)value & valid_mask);
	gpio_rockchip_write_we_pair(cfg->reg_base, RK_GPIO_SWPORT_DR_L,
				    RK_GPIO_SWPORT_DR_H,
				    valid_mask, next);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int gpio_rockchip_port_set_bits_raw(const struct device *dev,
					  gpio_port_pins_t pins)
{
	const struct gpio_rockchip_config *cfg = dev->config;
	struct gpio_rockchip_data *data = dev->data;
	uint32_t valid_pins = (uint32_t)pins & cfg->common.port_pin_mask;
	k_spinlock_key_t key;

	if (valid_pins == 0U) {
		return 0;
	}

	key = k_spin_lock(&data->lock);
	gpio_rockchip_write_we_pair(cfg->reg_base, RK_GPIO_SWPORT_DR_L,
				    RK_GPIO_SWPORT_DR_H,
				    valid_pins, valid_pins);
	k_spin_unlock(&data->lock, key);
	return 0;
}

static int gpio_rockchip_port_clear_bits_raw(const struct device *dev,
					    gpio_port_pins_t pins)
{
	const struct gpio_rockchip_config *cfg = dev->config;
	struct gpio_rockchip_data *data = dev->data;
	uint32_t valid_pins = (uint32_t)pins & cfg->common.port_pin_mask;
	k_spinlock_key_t key;

	if (valid_pins == 0U) {
		return 0;
	}

	key = k_spin_lock(&data->lock);
	gpio_rockchip_write_we_pair(cfg->reg_base, RK_GPIO_SWPORT_DR_L,
				    RK_GPIO_SWPORT_DR_H,
				    valid_pins, 0U);
	k_spin_unlock(&data->lock, key);
	return 0;
}

static int gpio_rockchip_port_toggle_bits(const struct device *dev, gpio_port_pins_t pins)
{
	const struct gpio_rockchip_config *cfg = dev->config;
	struct gpio_rockchip_data *data = dev->data;
	uint32_t valid_pins = (uint32_t)pins & cfg->common.port_pin_mask;
	uint32_t current;
	k_spinlock_key_t key;

	if (valid_pins == 0U) {
		return 0;
	}

	key = k_spin_lock(&data->lock);
	current = gpio_rockchip_read_pair(cfg->reg_base, RK_GPIO_SWPORT_DR_L,
				  RK_GPIO_SWPORT_DR_H);
	current ^= valid_pins;
	gpio_rockchip_write_we_pair(cfg->reg_base, RK_GPIO_SWPORT_DR_L,
				    RK_GPIO_SWPORT_DR_H,
				    valid_pins, current);
	k_spin_unlock(&data->lock, key);

	return 0;
}

static int gpio_rockchip_manage_callback(const struct device *dev,
					 struct gpio_callback *callback,
					 bool set)
{
	struct gpio_rockchip_data *data = dev->data;

	return gpio_manage_callback(&data->callbacks, callback, set);
}

static int gpio_rockchip_ensure_irq_connected(const struct device *dev)
{
	const struct gpio_rockchip_config *cfg = dev->config;
	struct gpio_rockchip_data *data = dev->data;
	unsigned int key;
	bool need_connect = false;

	key = irq_lock();
	if (!data->irq_connected) {
		data->irq_connected = true;
		need_connect = true;
	}
	irq_unlock(key);

	if (need_connect) {
		irq_connect_dynamic(cfg->irq_num, cfg->irq_prio,
				    (void (*)(const void *))gpio_rockchip_isr,
				    (const void *)dev, 0);
		irq_enable(cfg->irq_num);
	}

	return 0;
}

static int gpio_rockchip_pin_interrupt_configure(const struct device *dev,
					 gpio_pin_t pin,
					 enum gpio_int_mode mode,
					 enum gpio_int_trig trig)
{
	const struct gpio_rockchip_config *cfg = dev->config;
	struct gpio_rockchip_data *data = dev->data;
	const uint32_t pin_mask = BIT(pin);
	uint32_t int_type_val = 0U;
	uint32_t int_polarity_val = 0U;
	uint32_t int_bothedge_val = 0U;
	k_spinlock_key_t key;

	if (!gpio_rockchip_pin_valid(cfg, pin)) {
		return -EINVAL;
	}

	if (mode == GPIO_INT_MODE_DISABLED) {
		key = k_spin_lock(&data->lock);
		gpio_rockchip_write_we_pair(cfg->reg_base, RK_GPIO_INT_MASK_L,
					    RK_GPIO_INT_MASK_H,
					    pin_mask, pin_mask);
		gpio_rockchip_write_we_pair(cfg->reg_base, RK_GPIO_INT_EN_L,
					    RK_GPIO_INT_EN_H,
					    pin_mask, 0U);
		gpio_rockchip_write_we_pair(cfg->reg_base, RK_GPIO_INT_BOTHEDGE_L,
					    RK_GPIO_INT_BOTHEDGE_H,
					    pin_mask, 0U);
		k_spin_unlock(&data->lock, key);
		return 0;
	}

	if (mode == GPIO_INT_MODE_LEVEL) {
		if (trig == GPIO_INT_TRIG_LOW) {
			int_type_val = 0U;
			int_polarity_val = 0U;
		} else if (trig == GPIO_INT_TRIG_HIGH) {
			int_type_val = 0U;
			int_polarity_val = pin_mask;
		} else {
			return -ENOTSUP;
		}
		int_bothedge_val = 0U;
	} else if (mode == GPIO_INT_MODE_EDGE) {
		if (trig == GPIO_INT_TRIG_LOW) {
			int_type_val = pin_mask;
			int_polarity_val = 0U;
			int_bothedge_val = 0U;
		} else if (trig == GPIO_INT_TRIG_HIGH) {
			int_type_val = pin_mask;
			int_polarity_val = pin_mask;
			int_bothedge_val = 0U;
		} else if (trig == GPIO_INT_TRIG_BOTH) {
			int_type_val = pin_mask;
			int_polarity_val = pin_mask;
			int_bothedge_val = pin_mask;
		} else {
			return -ENOTSUP;
		}
	} else {
		return -ENOTSUP;
	}

	gpio_rockchip_ensure_irq_connected(dev);

	key = k_spin_lock(&data->lock);

	/* Reprogram under mask to avoid transient false trigger while changing mode. */
	gpio_rockchip_write_we_pair(cfg->reg_base, RK_GPIO_INT_MASK_L,
				    RK_GPIO_INT_MASK_H,
				    pin_mask, pin_mask);
	gpio_rockchip_write_we_pair(cfg->reg_base, RK_GPIO_INT_EN_L,
				    RK_GPIO_INT_EN_H,
				    pin_mask, 0U);

	gpio_rockchip_write_we_pair(cfg->reg_base, RK_GPIO_INT_TYPE_L,
				    RK_GPIO_INT_TYPE_H,
				    pin_mask, int_type_val);
	gpio_rockchip_write_we_pair(cfg->reg_base, RK_GPIO_INT_POLARITY_L,
				    RK_GPIO_INT_POLARITY_H,
				    pin_mask, int_polarity_val);
	gpio_rockchip_write_we_pair(cfg->reg_base, RK_GPIO_INT_BOTHEDGE_L,
				    RK_GPIO_INT_BOTHEDGE_H,
				    pin_mask, int_bothedge_val);

	/* EOI has effect on edge-type interrupts only per TRM; harmless for level. */
	gpio_rockchip_write_we_pair(cfg->reg_base, RK_GPIO_PORT_EOI_L,
				    RK_GPIO_PORT_EOI_H,
				    pin_mask, pin_mask);

	gpio_rockchip_write_we_pair(cfg->reg_base, RK_GPIO_INT_EN_L,
				    RK_GPIO_INT_EN_H,
				    pin_mask, pin_mask);
	gpio_rockchip_write_we_pair(cfg->reg_base, RK_GPIO_INT_MASK_L,
				    RK_GPIO_INT_MASK_H,
				    pin_mask, 0U);

	k_spin_unlock(&data->lock, key);

	return 0;
}

static uint32_t gpio_rockchip_get_pending_int(const struct device *dev)
{
	const struct gpio_rockchip_config *cfg = dev->config;

	return sys_read32(cfg->reg_base + RK_GPIO_INT_STATUS) & cfg->common.port_pin_mask;
}

static void gpio_rockchip_isr(const struct device *dev)
{
	const struct gpio_rockchip_config *cfg = dev->config;
	struct gpio_rockchip_data *data = dev->data;
	uint32_t status = sys_read32(cfg->reg_base + RK_GPIO_INT_STATUS) &
			  cfg->common.port_pin_mask;

	if (status == 0U) {
		return;
	}

	/* EOI clears edge interrupts only; level interrupts depend on input level/masking. */
	gpio_rockchip_write_we_pair(cfg->reg_base, RK_GPIO_PORT_EOI_L,
				    RK_GPIO_PORT_EOI_H,
				    status, status);

	gpio_fire_callbacks(&data->callbacks, dev, status);
}

static int gpio_rockchip_init(const struct device *dev)
{
	const struct gpio_rockchip_config *cfg = dev->config;
	struct gpio_rockchip_data *data = dev->data;

	sys_slist_init(&data->callbacks);
	data->irq_connected = false;

	/* Disable and mask all supported GPIO interrupts on init. */
	gpio_rockchip_write_we_pair(cfg->reg_base, RK_GPIO_INT_EN_L,
				    RK_GPIO_INT_EN_H,
				    cfg->common.port_pin_mask, 0U);
	gpio_rockchip_write_we_pair(cfg->reg_base, RK_GPIO_INT_MASK_L,
				    RK_GPIO_INT_MASK_H,
				    cfg->common.port_pin_mask,
				    cfg->common.port_pin_mask);
	/* Best-effort edge pending clear for all pins at startup. */
	gpio_rockchip_write_we_pair(cfg->reg_base, RK_GPIO_PORT_EOI_L,
				    RK_GPIO_PORT_EOI_H,
				    cfg->common.port_pin_mask,
				    cfg->common.port_pin_mask);

	return 0;
}

static DEVICE_API(gpio, gpio_rockchip_driver_api) = {
	.pin_configure = gpio_rockchip_pin_configure,
	.port_get_raw = gpio_rockchip_port_get_raw,
	.port_set_masked_raw = gpio_rockchip_port_set_masked_raw,
	.port_set_bits_raw = gpio_rockchip_port_set_bits_raw,
	.port_clear_bits_raw = gpio_rockchip_port_clear_bits_raw,
	.port_toggle_bits = gpio_rockchip_port_toggle_bits,
	.pin_interrupt_configure = gpio_rockchip_pin_interrupt_configure,
	.manage_callback = gpio_rockchip_manage_callback,
	.get_pending_int = gpio_rockchip_get_pending_int,
};

#define GPIO_ROCKCHIP_INIT(n) \
	static const struct gpio_rockchip_config gpio_rockchip_cfg_##n = { \
		.common = GPIO_COMMON_CONFIG_FROM_DT_INST(n), \
		.reg_base = DT_INST_REG_ADDR(n), \
		.bank = DT_INST_PROP(n, rockchip_bank), \
		.irq_num = DT_INST_IRQN(n), \
		.irq_prio = DT_INST_IRQ(n, priority), \
	}; \
	static struct gpio_rockchip_data gpio_rockchip_data_##n; \
	DEVICE_DT_INST_DEFINE(n, gpio_rockchip_init, NULL, \
			      &gpio_rockchip_data_##n, \
			      &gpio_rockchip_cfg_##n, \
			      POST_KERNEL, CONFIG_GPIO_INIT_PRIORITY, \
			      &gpio_rockchip_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GPIO_ROCKCHIP_INIT)


