/*
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT rockchip_rk_can

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/can/transceiver.h>
// #include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/check.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include "can_rockchip_priv.h"

LOG_MODULE_REGISTER(can_rockchip, CONFIG_CAN_LOG_LEVEL);

struct can_rockchip_filter {
	bool used;
	can_rx_callback_t cb;
	void *cb_arg;
	struct can_filter filter;
};

struct can_rockchip_config {
	struct can_driver_config common;
	mm_reg_t base;
	void (*irq_config_func)(const struct device *dev);
	uint32_t core_clock_hz;
	uint8_t max_filters;
};

struct can_rockchip_data {
	struct can_driver_data common;
	struct k_mutex lock;
	can_tx_callback_t tx_cbs[RKCAN_TX_FIFO_DEPTH];
	void *tx_user_data[RKCAN_TX_FIFO_DEPTH];
	uint8_t tx_head;
	uint8_t tx_tail;
	can_mode_t active_mode;
	enum can_state state;
	struct can_timing timing;
#ifdef CONFIG_CAN_FD_MODE
	struct can_timing timing_data;
#endif
	struct can_rockchip_filter filters[CONFIG_CAN_ROCKCHIP_MAX_FILTERS];
};

static inline uint32_t rkcan_read(const struct device *dev, uint32_t reg)
{
	const struct can_rockchip_config *cfg = dev->config;

	return sys_read32(cfg->base + reg);
}

static inline void rkcan_write(const struct device *dev, uint32_t reg, uint32_t val)
{
	const struct can_rockchip_config *cfg = dev->config;

	sys_write32(val, cfg->base + reg);
}

static inline uint8_t can_rockchip_get_tx_pending(const struct can_rockchip_data *data)
{
	return data->tx_head - data->tx_tail;
}

static enum can_state can_rockchip_state_from_hw(uint32_t state_reg, uint8_t rx_err, uint16_t tx_err)
{
	if ((state_reg & RKCAN_STATE_BUS_OFF) != 0U || tx_err >= 255U) {
		return CAN_STATE_BUS_OFF;
	}

	if (tx_err >= 128U || rx_err >= 128U) {
		return CAN_STATE_ERROR_PASSIVE;
	}

	if ((state_reg & RKCAN_STATE_ERROR_WARNING) != 0U || tx_err >= 96U || rx_err >= 96U) {
		return CAN_STATE_ERROR_WARNING;
	}

	return CAN_STATE_ERROR_ACTIVE;
}

static void can_rockchip_notify_state_change(const struct device *dev)
{
	struct can_rockchip_data *data = dev->data;
	struct can_bus_err_cnt err_cnt;
	enum can_state state;
	uint32_t state_reg;

	state_reg = rkcan_read(dev, RKCAN_STATE);
	err_cnt.rx_err_cnt = rkcan_read(dev, RKCAN_RXERRORCNT) & 0xffU;
	err_cnt.tx_err_cnt = rkcan_read(dev, RKCAN_TXERRORCNT) & 0x1ffU;

	state = can_rockchip_state_from_hw(state_reg, err_cnt.rx_err_cnt, err_cnt.tx_err_cnt);
	if (state == data->state) {
		return;
	}

	data->state = state;
	if (data->common.state_change_cb != NULL) {
		data->common.state_change_cb(dev, state, err_cnt, data->common.state_change_cb_user_data);
	}
}

static uint32_t can_rockchip_mode_to_reg(can_mode_t mode)
{
	uint32_t mode_reg = 0U;

	if ((mode & CAN_MODE_LOOPBACK) != 0U) {
		mode_reg |= RKCAN_MODE_LOOPBACK;
	}

	if ((mode & CAN_MODE_LISTENONLY) != 0U) {
		mode_reg |= RKCAN_MODE_SILENT;
	}

	if ((mode & CAN_MODE_ONE_SHOT) == 0U) {
		mode_reg |= RKCAN_MODE_AUTO_RETX;
	}

	/* Loopback/listen-only modes require self-test ACK behavior. */
	if ((mode_reg & (RKCAN_MODE_LOOPBACK | RKCAN_MODE_SILENT)) != 0U) {
		mode_reg |= RKCAN_MODE_SELF_TEST;
	}

	if (IS_ENABLED(CONFIG_CAN_MANUAL_RECOVERY_MODE) &&
	    (mode & CAN_MODE_MANUAL_RECOVERY) == 0U) {
		mode_reg |= RKCAN_MODE_AUTO_BUS_ON;
	}

	return mode_reg;
}

static void can_rockchip_rx_dispatch(const struct device *dev)
{
	struct can_rockchip_data *data = dev->data;
	struct can_frame frame = {0};
	uint32_t info;
	int i;

	info = rkcan_read(dev, RKCAN_RXFRAMEINFO);
	frame.flags = 0U;

	if ((info & RKCAN_FRAMEINFO_IDE) != 0U) {
		frame.flags |= CAN_FRAME_IDE;
	}

	if ((info & RKCAN_FRAMEINFO_RTR) != 0U) {
		frame.flags |= CAN_FRAME_RTR;
	}

	frame.dlc = info & RKCAN_FRAMEINFO_DLC_MASK;
	frame.id = rkcan_read(dev, RKCAN_RXID) & CAN_EXT_ID_MASK;

	if ((frame.flags & CAN_FRAME_IDE) == 0U) {
		frame.id &= CAN_STD_ID_MASK;
	}

	if ((frame.flags & CAN_FRAME_RTR) == 0U) {
		frame.data_32[0] = rkcan_read(dev, RKCAN_RXDATA0);
		frame.data_32[1] = rkcan_read(dev, RKCAN_RXDATA1);
	}

	for (i = 0; i < ARRAY_SIZE(data->filters); i++) {
		if (!data->filters[i].used) {
			continue;
		}

		if (!can_frame_matches_filter(&frame, &data->filters[i].filter)) {
			continue;
		}

		data->filters[i].cb(dev, &frame, data->filters[i].cb_arg);
	}
}

static can_mode_t can_rockchip_supported_modes(void)
{
	can_mode_t supported = CAN_MODE_NORMAL | CAN_MODE_LOOPBACK |
		CAN_MODE_LISTENONLY | CAN_MODE_ONE_SHOT;

	if (IS_ENABLED(CONFIG_CAN_MANUAL_RECOVERY_MODE)) {
		supported |= CAN_MODE_MANUAL_RECOVERY;
	}

	return supported;
}

static int can_rockchip_get_core_clock(const struct device *dev, uint32_t *rate)
{
	const struct can_rockchip_config *cfg = dev->config;

	*rate = cfg->core_clock_hz;

	return 0;
}

/*
 * Zephyr entry point: report controller capabilities.
 */
static int can_rockchip_get_capabilities(const struct device *dev, can_mode_t *cap)
{
	ARG_UNUSED(dev);

	*cap = can_rockchip_supported_modes();

	return 0;
}

static int can_rockchip_set_mode(const struct device *dev, can_mode_t mode)
{
	struct can_rockchip_data *data = dev->data;
	can_mode_t supported = can_rockchip_supported_modes();

	if ((mode & ~supported) != 0U) {
		return -ENOTSUP;
	}

	if (data->common.started) {
		return -EBUSY;
	}

	data->active_mode = mode;
	data->common.mode = mode;

	return 0;
}

static int can_rockchip_set_timing(const struct device *dev, const struct can_timing *timing)
{
	struct can_rockchip_data *data = dev->data;
	uint32_t bt;

	if (timing == NULL) {
		return -EINVAL;
	}

	if (data->common.started) {
		return -EBUSY;
	}

	if (timing->sjw < 1U || timing->sjw > 4U ||
	    timing->prescaler < 1U || timing->prescaler > 64U ||
	    timing->phase_seg1 < 1U || timing->phase_seg1 > 16U ||
	    timing->phase_seg2 < 1U || timing->phase_seg2 > 8U ||
	    timing->prop_seg != 0U) {
		return -EINVAL;
	}

	data->timing = *timing;

	bt = FIELD_PREP(RKCAN_BITTIMING_SJW_MASK, timing->sjw - 1U) |
	     FIELD_PREP(RKCAN_BITTIMING_BRP_MASK, timing->prescaler - 1U) |
	     FIELD_PREP(RKCAN_BITTIMING_TSEG2_MASK, timing->phase_seg2 - 1U) |
	     FIELD_PREP(RKCAN_BITTIMING_TSEG1_MASK, timing->phase_seg1 - 1U);

	if ((data->active_mode & CAN_MODE_3_SAMPLES) != 0U) {
		bt |= RKCAN_BITTIMING_SAMPLE_MODE;
	}

	rkcan_write(dev, RKCAN_BITTIMING, bt);
	return 0;
}

#ifdef CONFIG_CAN_FD_MODE
/*
 * Configure data phase timing (CAN FD only).
 *
 * Keep returning -ENOTSUP until the IP has verified FD capability and the
 * register programming path is implemented.
 */
static int can_rockchip_set_timing_data(const struct device *dev,
					const struct can_timing *timing_data)
{
	struct can_rockchip_data *data = dev->data;

	if (timing_data == NULL) {
		return -EINVAL;
	}

	if (data->common.started) {
		return -EBUSY;
	}

	/*
	 * TODO: If the IP supports CAN FD, implement DBTP/TDC programming and
	 * advertise CAN_MODE_FD from get_capabilities().
	 */
	data->timing_data = *timing_data;
	return -ENOTSUP;
}
#endif

static int can_rockchip_start(const struct device *dev)
{
	const struct can_rockchip_config *cfg = dev->config;
	struct can_rockchip_data *data = dev->data;
	uint32_t mode_reg;
	int err;

	if (data->common.started) {
		return -EALREADY;
	}

	if (cfg->common.phy != NULL) {
		err = can_transceiver_enable(cfg->common.phy, data->common.mode);
		if (err != 0) {
			LOG_ERR("failed to enable CAN transceiver (err %d)", err);
			return err;
		}
	}

	/* Apply configured operation mode and enter working state. */
	mode_reg = can_rockchip_mode_to_reg(data->active_mode);
	rkcan_write(dev, RKCAN_MODE, mode_reg | RKCAN_MODE_WORK);

	/* Clear any pending interrupts and unmask the core CAN events. */
	rkcan_write(dev, RKCAN_INT, 0x7fffU);
	rkcan_write(dev, RKCAN_INT_MASK,
		   ~(RKCAN_INT_RX_FINISH | RKCAN_INT_TX_FINISH |
		     RKCAN_INT_ERROR_WARNING | RKCAN_INT_PASSIVE_ERROR |
		     RKCAN_INT_ARB_FAIL | RKCAN_INT_ERROR |
		     RKCAN_INT_BUS_OFF | RKCAN_INT_BUS_OFF_RECOVERY) & 0x7fffU);

	data->state = CAN_STATE_ERROR_ACTIVE;
	data->common.started = true;

	return 0;
}

static int can_rockchip_stop(const struct device *dev)
{
	const struct can_rockchip_config *cfg = dev->config;
	struct can_rockchip_data *data = dev->data;
	can_tx_callback_t tx_cbs[RKCAN_TX_FIFO_DEPTH] = {0};
	void *tx_user_data[RKCAN_TX_FIFO_DEPTH] = {0};
	uint8_t tx_pending;
	uint8_t i;
	int err;

	if (!data->common.started) {
		return -EALREADY;
	}

	/* Enter configuration/idle mode. */
	rkcan_write(dev, RKCAN_MODE, can_rockchip_mode_to_reg(data->active_mode));
	rkcan_write(dev, RKCAN_INT_MASK, 0x7fffU);
	data->common.started = false;
	data->state = CAN_STATE_STOPPED;

	k_mutex_lock(&data->lock, K_FOREVER);
	tx_pending = can_rockchip_get_tx_pending(data);
	for (i = 0U; i < tx_pending; i++) {
		uint8_t idx = (data->tx_tail + i) & (RKCAN_TX_FIFO_DEPTH - 1U);

		tx_cbs[i] = data->tx_cbs[idx];
		tx_user_data[i] = data->tx_user_data[idx];
		data->tx_cbs[idx] = NULL;
		data->tx_user_data[idx] = NULL;
	}
	data->tx_tail += tx_pending;
	k_mutex_unlock(&data->lock);

	for (i = 0U; i < tx_pending; i++) {
		if (tx_cbs[i] != NULL) {
			tx_cbs[i](dev, -ENETDOWN, tx_user_data[i]);
		}
	}

	if (cfg->common.phy != NULL) {
		err = can_transceiver_disable(cfg->common.phy);
		if (err != 0) {
			LOG_ERR("failed to disable CAN transceiver (err %d)", err);
			return err;
		}
	}

	return 0;
}

static int can_rockchip_send(const struct device *dev, const struct can_frame *frame,
			     k_timeout_t timeout, can_tx_callback_t callback, void *user_data)
{
	struct can_rockchip_data *data = dev->data;
	uint32_t info = 0U;
	uint8_t tx_head;
	uint8_t nbytes;

	ARG_UNUSED(timeout);

	if (!data->common.started) {
		return -ENETDOWN;
	}

	if (frame == NULL) {
		return -EINVAL;
	}

	nbytes = can_dlc_to_bytes(frame->dlc);
	if (nbytes > CAN_MAX_DLEN) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	if (can_rockchip_get_tx_pending(data) >= RKCAN_TX_FIFO_DEPTH ||
	    (rkcan_read(dev, RKCAN_STATE) & RKCAN_STATE_TX_BUF_FULL) != 0U) {
		k_mutex_unlock(&data->lock);
		return -EAGAIN;
	}

	tx_head = data->tx_head & (RKCAN_TX_FIFO_DEPTH - 1U);
	data->tx_cbs[tx_head] = callback;
	data->tx_user_data[tx_head] = user_data;

	if ((frame->flags & CAN_FRAME_IDE) != 0U) {
		info |= RKCAN_FRAMEINFO_IDE;
	}

	if ((frame->flags & CAN_FRAME_RTR) != 0U) {
		info |= RKCAN_FRAMEINFO_RTR;
	}

	info |= frame->dlc & RKCAN_FRAMEINFO_DLC_MASK;
	rkcan_write(dev, RKCAN_TXFRAMEINFO, info);
	rkcan_write(dev, RKCAN_TXID, frame->id & CAN_EXT_ID_MASK);

	if ((frame->flags & CAN_FRAME_RTR) == 0U) {
		rkcan_write(dev, RKCAN_TXDATA0, frame->data_32[0]);
		rkcan_write(dev, RKCAN_TXDATA1, frame->data_32[1]);
	}

	/* Submit this frame to the selected hardware TX buffer. */
	rkcan_write(dev, RKCAN_CMD, RKCAN_CMD_TX_REQ(tx_head));
	data->tx_head++;
	k_mutex_unlock(&data->lock);

	ARG_UNUSED(timeout);
	return 0;
}

static int can_rockchip_add_rx_filter(const struct device *dev, can_rx_callback_t callback,
				      void *user_data, const struct can_filter *filter)
{
	const struct can_rockchip_config *cfg = dev->config;
	struct can_rockchip_data *data = dev->data;
	int i;

	if ((callback == NULL) || (filter == NULL)) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	for (i = 0; i < cfg->max_filters; i++) {
		if (!data->filters[i].used) {
			data->filters[i].used = true;
			data->filters[i].cb = callback;
			data->filters[i].cb_arg = user_data;
			data->filters[i].filter = *filter;

			/* Hardware acceptance filters are configured as accept-all for now. */
			k_mutex_unlock(&data->lock);
			return i;
		}
	}

	k_mutex_unlock(&data->lock);
	return -ENOSPC;
}

static void can_rockchip_remove_rx_filter(const struct device *dev, int filter_id)
{
	const struct can_rockchip_config *cfg = dev->config;
	struct can_rockchip_data *data = dev->data;

	if ((filter_id < 0) || (filter_id >= cfg->max_filters)) {
		return;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	data->filters[filter_id].used = false;
	data->filters[filter_id].cb = NULL;
	data->filters[filter_id].cb_arg = NULL;
	(void)memset(&data->filters[filter_id].filter, 0, sizeof(data->filters[filter_id].filter));

	k_mutex_unlock(&data->lock);
}

static int can_rockchip_get_state(const struct device *dev, enum can_state *state,
				  struct can_bus_err_cnt *err_cnt)
{
	struct can_rockchip_data *data = dev->data;
	uint32_t state_reg;
	uint8_t rx_err;
	uint16_t tx_err;

	if (state != NULL) {
		if (!data->common.started) {
			*state = CAN_STATE_STOPPED;
		} else {
			state_reg = rkcan_read(dev, RKCAN_STATE);
			rx_err = rkcan_read(dev, RKCAN_RXERRORCNT) & 0xffU;
			tx_err = rkcan_read(dev, RKCAN_TXERRORCNT) & 0x1ffU;
			*state = can_rockchip_state_from_hw(state_reg, rx_err, tx_err);
		}
	}

	if (err_cnt != NULL) {
		err_cnt->rx_err_cnt = rkcan_read(dev, RKCAN_RXERRORCNT) & 0xffU;
		err_cnt->tx_err_cnt = rkcan_read(dev, RKCAN_TXERRORCNT) & 0x1ffU;
	}

	return 0;
}

#ifdef CONFIG_CAN_MANUAL_RECOVERY_MODE
/*
 * Manual bus-off recovery entry point.
 *
 * Implement only if the controller supports explicit recovery trigger and
 * completion detection.
 */
static int can_rockchip_recover(const struct device *dev, k_timeout_t timeout)
{
	struct can_rockchip_data *data = dev->data;
	int64_t end;

	if (!data->common.started) {
		return -ENETDOWN;
	}

	/* In manual mode, recover by returning to config mode and re-entering work mode. */
	rkcan_write(dev, RKCAN_MODE, can_rockchip_mode_to_reg(data->active_mode));
	rkcan_write(dev, RKCAN_MODE, can_rockchip_mode_to_reg(data->active_mode) | RKCAN_MODE_WORK);

	if (K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
		return 0;
	}

	end = k_uptime_ticks() + timeout.ticks;
	while ((rkcan_read(dev, RKCAN_STATE) & RKCAN_STATE_BUS_OFF) != 0U) {
		if (!K_TIMEOUT_EQ(timeout, K_FOREVER) && k_uptime_ticks() >= end) {
			return -EAGAIN;
		}

		k_sleep(K_MSEC(1));
	}

	return 0;
}
#endif

static void can_rockchip_set_state_change_callback(const struct device *dev,
					   can_state_change_callback_t callback,
					   void *user_data)
{
	struct can_rockchip_data *data = dev->data;

	data->common.state_change_cb = callback;
	data->common.state_change_cb_user_data = user_data;
}

static int can_rockchip_get_max_filters(const struct device *dev, bool ide)
{
	const struct can_rockchip_config *cfg = dev->config;

	ARG_UNUSED(ide);

	return cfg->max_filters;
}

static void can_rockchip_isr(const struct device *dev)
{
	struct can_rockchip_data *data = dev->data;
	can_tx_callback_t tx_cb = NULL;
	void *tx_user_data = NULL;
	uint8_t tx_tail;
	int tx_status = 0;
	uint32_t isr;

	isr = rkcan_read(dev, RKCAN_INT);
	if (isr == 0U) {
		return;
	}

	/* Clear latched interrupt bits (W1C). */
	rkcan_write(dev, RKCAN_INT, isr & 0x7fffU);

	if ((isr & RKCAN_INT_RX_FINISH) != 0U) {
		can_rockchip_rx_dispatch(dev);
	}

	if ((isr & RKCAN_INT_TX_FINISH) != 0U) {
		tx_status = 0;
	}

	if ((isr & (RKCAN_INT_ARB_FAIL | RKCAN_INT_ERROR | RKCAN_INT_BUS_OFF)) != 0U) {
		tx_status = (isr & RKCAN_INT_BUS_OFF) ? -ENETUNREACH : -EIO;
	}

	if ((isr & (RKCAN_INT_TX_FINISH | RKCAN_INT_ARB_FAIL | RKCAN_INT_ERROR | RKCAN_INT_BUS_OFF)) != 0U) {
		k_mutex_lock(&data->lock, K_FOREVER);
		if (can_rockchip_get_tx_pending(data) > 0U) {
			tx_tail = data->tx_tail & (RKCAN_TX_FIFO_DEPTH - 1U);
			tx_cb = data->tx_cbs[tx_tail];
			tx_user_data = data->tx_user_data[tx_tail];
			data->tx_cbs[tx_tail] = NULL;
			data->tx_user_data[tx_tail] = NULL;
			data->tx_tail++;
		}
		k_mutex_unlock(&data->lock);
	}

	if (tx_cb != NULL) {
		tx_cb(dev, tx_status, tx_user_data);
	}

	if ((isr & (RKCAN_INT_ERROR_WARNING | RKCAN_INT_PASSIVE_ERROR |
		    RKCAN_INT_ERROR | RKCAN_INT_BUS_OFF |
		    RKCAN_INT_BUS_OFF_RECOVERY)) != 0U) {
		can_rockchip_notify_state_change(dev);
	}
}

static int can_rockchip_init(const struct device *dev)
{
    const struct can_rockchip_config *cfg = dev->config;
    struct can_rockchip_data *data = dev->data;
    // int err;

    if (cfg->max_filters > ARRAY_SIZE(data->filters)) {
        LOG_ERR("max-filters (%u) exceeds build limit (%u)", cfg->max_filters,
            (unsigned int)ARRAY_SIZE(data->filters));
        return -EINVAL;
    }

    /*
     * Bao hypervisor Zephyr VM assumption:
     * pin mux / IO domain / clock / reset are pre-configured by U-Boot
     * before entering the guest. The guest driver only programs CAN IP
     * registers and IRQ, and must not rely on local clock/reset providers.
     *
     * Keep pinctrl optional for non-VM reuse; in VM deployments pinctrl
     * can be omitted from DT and init continues normally.
     */
    // if (cfg->pcfg != NULL) {
    //     err = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
    //     if (err != 0) {
    //         LOG_ERR("pinctrl setup failed (err %d)", err);
    //         return err;
    //     }
    // } else {
    //     LOG_DBG("skip pinctrl: assumed pre-configured by bootloader/hypervisor");
    // }

    k_mutex_init(&data->lock);
    (void)memset(data->tx_cbs, 0, sizeof(data->tx_cbs));
    (void)memset(data->tx_user_data, 0, sizeof(data->tx_user_data));
    data->tx_head = 0U;
    data->tx_tail = 0U;
    data->common.mode = CAN_MODE_NORMAL;
    data->active_mode = CAN_MODE_NORMAL;
    data->state = CAN_STATE_STOPPED;

    /* Accept all frames at hardware level, software filters do final dispatch. */
    rkcan_write(dev, RKCAN_IDCODE, 0U);
    rkcan_write(dev, RKCAN_IDMASK, 0U);
    rkcan_write(dev, RKCAN_INT_MASK, 0x7fffU);
    rkcan_write(dev, RKCAN_INT, 0x7fffU);

    if (cfg->irq_config_func != NULL) {
        cfg->irq_config_func(dev);
    }

    /*
     * Intentionally no local pin/clock/reset sequencing here:
     * those platform resources are expected to stay enabled by firmware
     * for the lifetime of this VM.
     */
    return 0;
}

static DEVICE_API(can, can_rockchip_api) = {
	.get_capabilities = can_rockchip_get_capabilities,
	.start = can_rockchip_start,
	.stop = can_rockchip_stop,
	.set_mode = can_rockchip_set_mode,
	.set_timing = can_rockchip_set_timing,
	.send = can_rockchip_send,
	.add_rx_filter = can_rockchip_add_rx_filter,
	.remove_rx_filter = can_rockchip_remove_rx_filter,
	.get_state = can_rockchip_get_state,
#ifdef CONFIG_CAN_MANUAL_RECOVERY_MODE
	.recover = can_rockchip_recover,
#endif
	.set_state_change_callback = can_rockchip_set_state_change_callback,
	.get_core_clock = can_rockchip_get_core_clock,
	.get_max_filters = can_rockchip_get_max_filters,
	.timing_min = {
		.sjw = 1,
		.prop_seg = 0,
		.phase_seg1 = 1,
		.phase_seg2 = 1,
		.prescaler = 1,
	},
	.timing_max = {
		.sjw = 4,
		.prop_seg = 0,
		.phase_seg1 = 16,
		.phase_seg2 = 8,
		.prescaler = 64,
	},
#ifdef CONFIG_CAN_FD_MODE
	.set_timing_data = can_rockchip_set_timing_data,
	.timing_data_min = {
		.sjw = 1,
		.prop_seg = 0,
		.phase_seg1 = 1,
		.phase_seg2 = 1,
		.prescaler = 1,
	},
	.timing_data_max = {
		.sjw = 16,
		.prop_seg = 0,
		.phase_seg1 = 32,
		.phase_seg2 = 16,
		.prescaler = 1024,
	},
#endif
};

#define ROCKCHIP_CAN_INIT(inst)                                                                   \
                                                                                                   \
	static void can_rockchip_irq_configure_##inst(const struct device *dev)                   \
	{                                                                                          \
		ARG_UNUSED(dev);                                                                     \
		/* If the controller exposes multiple lines, connect them here as well. */         \
		IRQ_CONNECT(DT_INST_IRQN(inst), DT_INST_IRQ(inst, priority),                        \
			    can_rockchip_isr, DEVICE_DT_INST_GET(inst), 0);                          \
		irq_enable(DT_INST_IRQN(inst));                                                      \
	}                                                                                          \
                                                                                                   \
	static const struct can_rockchip_config can_rockchip_config_##inst = {                    \
		.common = CAN_DT_DRIVER_CONFIG_INST_GET(inst, ROCKCHIP_CAN_MIN_BITRATE,            \
						      ROCKCHIP_CAN_MAX_BITRATE),                    \
		.base = DT_INST_REG_ADDR(inst),                                                      \
		.irq_config_func = can_rockchip_irq_configure_##inst,                                \
		.core_clock_hz = DT_INST_PROP_OR(inst, clock_frequency, 80000000U),                  \
		.max_filters = DT_INST_PROP_OR(inst, max_filters, CONFIG_CAN_ROCKCHIP_MAX_FILTERS),  \
	};                                                                                         \
                                                                                                   \
	static struct can_rockchip_data can_rockchip_data_##inst;                                  \
                                                                                                   \
	CAN_DEVICE_DT_INST_DEFINE(inst, can_rockchip_init, NULL, &can_rockchip_data_##inst,       \
				  &can_rockchip_config_##inst, POST_KERNEL,                           \
				  CONFIG_CAN_INIT_PRIORITY, &can_rockchip_api);

DT_INST_FOREACH_STATUS_OKAY(ROCKCHIP_CAN_INIT)
