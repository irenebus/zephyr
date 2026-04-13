/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT rockchip_rk3588_spi

#define LOG_LEVEL CONFIG_SPI_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(spi_rockchip);

#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include "spi_context.h"

/* SPI register offsets */
#define RK_SPI_CTRLR0            0x0000
#define RK_SPI_CTRLR1            0x0004
#define RK_SPI_SSIENR            0x0008
#define RK_SPI_SER               0x000c
#define RK_SPI_BAUDR             0x0010
#define RK_SPI_SR                0x0024
#define RK_SPI_IMR               0x002c
#define RK_SPI_ICR               0x0038
#define RK_SPI_TXDR              0x0400
#define RK_SPI_RXDR              0x0800

/* CTRLR0 fields */
#define RK_CR0_DFS_OFFSET        0
#define RK_CR0_DFS_8BIT          0x1
#define RK_CR0_SCPH_OFFSET       6
#define RK_CR0_SCPOL_OFFSET      7
#define RK_CR0_EM_OFFSET         11
#define RK_CR0_EM_BIG            0x1
#define RK_CR0_BHT_OFFSET        13
#define RK_CR0_BHT_8BIT          0x1
#define RK_CR0_XFM_OFFSET        18
#define RK_CR0_XFM_TR            0x0
#define RK_CR0_XFM_TO            0x1
#define RK_CR0_XFM_RO            0x2

/* SR fields */
#define RK_SR_BUSY               BIT(0)
#define RK_SR_TF_FULL            BIT(1)
#define RK_SR_RF_EMPTY           BIT(3)

#define RK_SPI_TIMEOUT_US        20000
#define RK_SPI_MAX_FRAMES        0x10000

struct rk_spi_config {
	DEVICE_MMIO_ROM;
	uint32_t clock_frequency;
	uint8_t num_cs;

#ifdef CONFIG_PINCTRL
	const struct pinctrl_dev_config *pcfg;
#endif
};

struct rk_spi_data {
	DEVICE_MMIO_RAM;
	struct spi_context ctx;
	uint32_t ctrlr0;
};

static inline mem_addr_t rk_spi_base(const struct device *dev)
{
	return DEVICE_MMIO_GET(dev);
}

static inline uint32_t rk_spi_read(const struct device *dev, uint32_t reg)
{
	return sys_read32(rk_spi_base(dev) + reg);
}

static inline void rk_spi_write(const struct device *dev, uint32_t reg, uint32_t val)
{
	sys_write32(val, rk_spi_base(dev) + reg);
}

static int rk_spi_wait_while(const struct device *dev, uint32_t mask)
{
	uint32_t t = RK_SPI_TIMEOUT_US;

	while ((rk_spi_read(dev, RK_SPI_SR) & mask) != 0U) {
		if (t-- == 0U) {
			return -ETIMEDOUT;
		}
		k_busy_wait(1);
	}

	return 0;
}

static inline void rk_spi_chip_enable(const struct device *dev, bool enable)
{
	rk_spi_write(dev, RK_SPI_SSIENR, enable ? 1U : 0U);
}

static inline void rk_spi_set_native_cs(const struct spi_config *config, const struct device *dev,
					bool enable)
{
	if (spi_cs_is_gpio(config)) {
		return;
	}

	rk_spi_write(dev, RK_SPI_SER, enable ? BIT(config->slave) : 0U);
}

static int rk_spi_configure(const struct device *dev, const struct spi_config *config)
{
	const struct rk_spi_config *cfg = dev->config;
	struct rk_spi_data *data = dev->data;
	uint32_t operation = config->operation;
	uint32_t cr0;
	uint32_t div;

	if (spi_context_configured(&data->ctx, config)) {
		return 0;
	}

	if ((operation & SPI_OP_MODE_SLAVE) != 0U) {
		return -ENOTSUP;
	}

	if (SPI_WORD_SIZE_GET(operation) != 8U) {
		return -ENOTSUP;
	}

	if ((operation & SPI_TRANSFER_LSB) != 0U) {
		return -ENOTSUP;
	}

	if ((operation & SPI_HALF_DUPLEX) != 0U) {
		return -ENOTSUP;
	}

	if ((operation & SPI_FRAME_FORMAT_TI) != 0U) {
		return -ENOTSUP;
	}

	if (IS_ENABLED(CONFIG_SPI_EXTENDED_MODES) && ((operation & SPI_LINES_MASK) != SPI_LINES_SINGLE)) {
		return -ENOTSUP;
	}

	if ((operation & SPI_CS_ACTIVE_HIGH) != 0U && !spi_cs_is_gpio(config)) {
		return -ENOTSUP;
	}

	if (config->slave >= cfg->num_cs) {
		return -EINVAL;
	}

	if (config->frequency == 0U || cfg->clock_frequency == 0U) {
		return -EINVAL;
	}

	cr0 = (RK_CR0_DFS_8BIT << RK_CR0_DFS_OFFSET) |
	      (RK_CR0_EM_BIG << RK_CR0_EM_OFFSET) |
	      (RK_CR0_BHT_8BIT << RK_CR0_BHT_OFFSET);

	if ((operation & SPI_MODE_CPHA) != 0U) {
		cr0 |= BIT(RK_CR0_SCPH_OFFSET);
	}

	if ((operation & SPI_MODE_CPOL) != 0U) {
		cr0 |= BIT(RK_CR0_SCPOL_OFFSET);
	}

	div = DIV_ROUND_UP(cfg->clock_frequency, config->frequency);
	if (div < 2U) {
		div = 2U;
	}
	if ((div & 0x1U) != 0U) {
		div++;
	}
	if (div > 0xfffeU) {
		div = 0xfffeU;
	}

	rk_spi_chip_enable(dev, false);
	rk_spi_write(dev, RK_SPI_BAUDR, div);

	data->ctrlr0 = cr0;
	data->ctx.config = config;

	return 0;
}

static int rk_spi_poll_xfer(const struct device *dev)
{
	struct rk_spi_data *data = dev->data;
	struct spi_context *ctx = &data->ctx;
	size_t frames = MAX(spi_context_total_tx_len(ctx), spi_context_total_rx_len(ctx));
	bool tx_on;
	bool rx_on;
	uint32_t xfm;
	int ret;

	if (frames == 0U) {
		return 0;
	}

	if (frames > RK_SPI_MAX_FRAMES) {
		return -EMSGSIZE;
	}

	tx_on = spi_context_tx_on(ctx);
	rx_on = spi_context_rx_on(ctx);

	if (tx_on && rx_on) {
		xfm = RK_CR0_XFM_TR;
	} else if (rx_on) {
		xfm = RK_CR0_XFM_RO;
	} else {
		xfm = RK_CR0_XFM_TO;
	}

	rk_spi_chip_enable(dev, false);
	rk_spi_write(dev, RK_SPI_CTRLR0, data->ctrlr0 | (xfm << RK_CR0_XFM_OFFSET));
	rk_spi_write(dev, RK_SPI_CTRLR1, (uint32_t)(frames - 1U));
	rk_spi_write(dev, RK_SPI_ICR, 0xffffffffU);
	rk_spi_chip_enable(dev, true);

	while (spi_context_tx_on(ctx) || spi_context_rx_on(ctx)) {
		uint8_t txd = 0xff;
		uint8_t rxd;

		if (spi_context_tx_buf_on(ctx)) {
			txd = *((const uint8_t *)ctx->tx_buf);
		}

		ret = rk_spi_wait_while(dev, RK_SR_TF_FULL);
		if (ret < 0) {
			return ret;
		}

		rk_spi_write(dev, RK_SPI_TXDR, txd);

		/* RX FIFO is not expected to produce data in TX-only mode. */
		if (xfm != RK_CR0_XFM_TO) {
			ret = rk_spi_wait_while(dev, RK_SR_RF_EMPTY);
			if (ret < 0) {
				return ret;
			}

			rxd = (uint8_t)rk_spi_read(dev, RK_SPI_RXDR);
			if (spi_context_rx_buf_on(ctx)) {
				*((uint8_t *)ctx->rx_buf) = rxd;
			}

			spi_context_update_rx(ctx, 1, 1);
		}

		spi_context_update_tx(ctx, 1, 1);
	}

	ret = rk_spi_wait_while(dev, RK_SR_BUSY);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static int rk_spi_transceive(const struct device *dev, const struct spi_config *config,
			     const struct spi_buf_set *tx_bufs,
			     const struct spi_buf_set *rx_bufs)
{
	struct rk_spi_data *data = dev->data;
	bool cs_active = false;
	int ret;

	spi_context_lock(&data->ctx, false, NULL, NULL, config);

	ret = rk_spi_configure(dev, config);
	if (ret < 0) {
		goto out;
	}

	spi_context_buffers_setup(&data->ctx, tx_bufs, rx_bufs, 1);
	if (!(spi_context_tx_on(&data->ctx) || spi_context_rx_on(&data->ctx))) {
		ret = 0;
		goto out;
	}

	rk_spi_set_native_cs(config, dev, true);
	if (spi_cs_is_gpio(config)) {
		spi_context_cs_control(&data->ctx, true);
	}
	cs_active = true;

	ret = rk_spi_poll_xfer(dev);

out:
	rk_spi_chip_enable(dev, false);
	if (cs_active) {
		if (spi_cs_is_gpio(config)) {
			spi_context_cs_control(&data->ctx, false);
		}
		rk_spi_set_native_cs(config, dev, false);
	}

	spi_context_release(&data->ctx, ret);
	return ret;
}

#ifdef CONFIG_SPI_ASYNC
static int rk_spi_transceive_async(const struct device *dev, const struct spi_config *config,
				   const struct spi_buf_set *tx_bufs,
				   const struct spi_buf_set *rx_bufs,
				   spi_callback_t cb,
				   void *userdata)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(config);
	ARG_UNUSED(tx_bufs);
	ARG_UNUSED(rx_bufs);
	ARG_UNUSED(cb);
	ARG_UNUSED(userdata);

	return -ENOTSUP;
}
#endif

static int rk_spi_release(const struct device *dev, const struct spi_config *config)
{
	struct rk_spi_data *data = dev->data;

	if (!spi_context_configured(&data->ctx, config)) {
		return -EINVAL;
	}

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

static DEVICE_API(spi, rk_spi_api) = {
	.transceive = rk_spi_transceive,
#ifdef CONFIG_SPI_ASYNC
	.transceive_async = rk_spi_transceive_async,
#endif
#ifdef CONFIG_SPI_RTIO
	.iodev_submit = spi_rtio_iodev_default_submit,
#endif
	.release = rk_spi_release,
};

static int rk_spi_init(const struct device *dev)
{
	const struct rk_spi_config *cfg = dev->config;
	struct rk_spi_data *data = dev->data;
	int ret;


#ifdef CONFIG_PINCTRL
	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}
#endif

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

	rk_spi_chip_enable(dev, false);
	rk_spi_write(dev, RK_SPI_IMR, 0U);
	rk_spi_write(dev, RK_SPI_ICR, 0xffffffffU);
	rk_spi_write(dev, RK_SPI_SER, 0U);

	ret = spi_context_cs_configure_all(&data->ctx);
	if (ret < 0) {
		return ret;
	}

	spi_context_unlock_unconditionally(&data->ctx);
	return 0;
}

#define RK_SPI_INIT(inst)                                                                     \
	IF_ENABLED(CONFIG_PINCTRL, (PINCTRL_DT_INST_DEFINE(inst);))                            \
	static struct rk_spi_data rk_spi_data_##inst = {                                       \
		SPI_CONTEXT_INIT_LOCK(rk_spi_data_##inst, ctx),                                   \
		SPI_CONTEXT_INIT_SYNC(rk_spi_data_##inst, ctx),                                   \
		SPI_CONTEXT_CS_GPIOS_INITIALIZE(DT_DRV_INST(inst), ctx)                           \
	};                                                                                         \
	static const struct rk_spi_config rk_spi_config_##inst = {                                 \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(inst)),                                          \
		.clock_frequency = DT_INST_PROP_OR(inst, clock_frequency, 200000000),             \
		.num_cs = DT_INST_PROP_OR(inst, num_cs, 2),                                       \
		IF_ENABLED(CONFIG_PINCTRL, (.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst),))       \
	};                                                                                         \
	SPI_DEVICE_DT_INST_DEFINE(inst, rk_spi_init, NULL, &rk_spi_data_##inst,                   \
				  &rk_spi_config_##inst, POST_KERNEL, CONFIG_SPI_INIT_PRIORITY,      \
				  &rk_spi_api);

DT_INST_FOREACH_STATUS_OKAY(RK_SPI_INIT)
