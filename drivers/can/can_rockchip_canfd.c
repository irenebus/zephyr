/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Rockchip RK3588 CAN-FD controller driver, classic CAN only.
 *
 * The controller has two TX mailboxes, a 19-word RX FIFO element (three
 * header words followed by 16 data words) and no general purpose RX filter
 * list. TX completion is detected through the RXSTX self-reception mode,
 * which is also required by the erratum that may mangle extended frames.
 */

#define DT_DRV_COMPAT rockchip_rk3588_canfd

#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/can/transceiver.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(can_rockchip_canfd, CONFIG_CAN_LOG_LEVEL);

/* Registers */
#define RKCANFD_MODE			0x000U
#define RKCANFD_MODE_CAN_FD		BIT(15)
#define RKCANFD_MODE_AUTO_RETX		BIT(10)
#define RKCANFD_MODE_COVER		BIT(8)
#define RKCANFD_MODE_RXSTX		BIT(5)
#define RKCANFD_MODE_LBACK		BIT(4)
#define RKCANFD_MODE_SILENT		BIT(3)
#define RKCANFD_MODE_SELF_TEST		BIT(2)
#define RKCANFD_MODE_WORK		BIT(0)

#define RKCANFD_CMD			0x004U
#define RKCANFD_CMD_TX(n)		BIT(n)

#define RKCANFD_STATE			0x008U
#define RKCANFD_STATE_BUS_OFF		BIT(5)
#define RKCANFD_STATE_ERROR_WARNING	BIT(4)

#define RKCANFD_INT			0x00cU
#define RKCANFD_INT_TIMESTAMP_OVF	BIT(11)
#define RKCANFD_INT_BUS_OFF_RECOVERY	BIT(10)
#define RKCANFD_INT_BUS_OFF		BIT(9)
#define RKCANFD_INT_RX_FIFO_OVERFLOW	BIT(8)
#define RKCANFD_INT_ERROR		BIT(6)
#define RKCANFD_INT_TX_ARBIT_FAIL	BIT(5)
#define RKCANFD_INT_PASSIVE_ERROR	BIT(4)
#define RKCANFD_INT_OVERLOAD		BIT(3)
#define RKCANFD_INT_ERROR_WARNING	BIT(2)
#define RKCANFD_INT_TX_FINISH		BIT(1)
#define RKCANFD_INT_RX_FINISH		BIT(0)
#define RKCANFD_INT_ALL			GENMASK(14, 0)

#define RKCANFD_INT_MASK		0x010U

#define RKCANFD_FD_NOMINAL_BITTIMING	0x100U
#define RKCANFD_FD_BT_SJW		GENMASK(30, 24)
#define RKCANFD_FD_BT_BRP		GENMASK(23, 16)
#define RKCANFD_FD_BT_TSEG2		GENMASK(14, 8)
#define RKCANFD_FD_BT_TSEG1		GENMASK(7, 0)

#define RKCANFD_RXERRORCNT		0x034U
#define RKCANFD_RXERRORCNT_MASK		GENMASK(7, 0)
#define RKCANFD_TXERRORCNT		0x038U
#define RKCANFD_TXERRORCNT_MASK		GENMASK(8, 0)

#define RKCANFD_IDCODE			0x03cU
#define RKCANFD_IDMASK			0x040U
#define RKCANFD_IDMASK_ALL		0x1fffffffU

#define RKCANFD_FD_TXFRAMEINFO		0x200U
#define RKCANFD_FRAMEINFO_EFF		BIT(7)
#define RKCANFD_FRAMEINFO_RTR		BIT(6)
#define RKCANFD_FRAMEINFO_FDF		BIT(5)
#define RKCANFD_FRAMEINFO_DLC		GENMASK(3, 0)

#define RKCANFD_FD_TXID			0x204U
#define RKCANFD_FD_ID_EFF		GENMASK(28, 0)
#define RKCANFD_FD_ID_SFF		GENMASK(10, 0)
#define RKCANFD_FD_TXDATA(n)		(0x208U + ((n) * 4U))

#define RKCANFD_RX_FIFO_CTRL		0x118U
#define RKCANFD_RX_FIFO_CNT		GENMASK(7, 5)
#define RKCANFD_RX_FIFO_ENABLE		BIT(0)

#define RKCANFD_RX_FIFO_RDATA		0x400U

#define RKCANFD_TX_MAILBOXES		2U
#define RKCANFD_TX_DATA_WORDS		16U
#define RKCANFD_RX_DATA_WORDS		16U

#define RKCANFD_PRESCALER_MIN		2U
#define RKCANFD_PRESCALER_MAX		512U
#define RKCANFD_TSEG1_MAX		256U
#define RKCANFD_TSEG2_MAX		128U
#define RKCANFD_SJW_MAX			128U
#define RKCANFD_TQ_MAX			(1U + RKCANFD_TSEG1_MAX + RKCANFD_TSEG2_MAX)

struct rkcanfd_tx_frame {
	struct can_frame frame;
	can_tx_callback_t callback;
	void *user_data;
};

struct rkcanfd_filter {
	struct can_filter filter;
	can_rx_callback_t callback;
	void *user_data;
};

struct rkcanfd_config {
	struct can_driver_config common;
	uintptr_t phys;
	size_t size;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	const struct device *pclk_dev;
	clock_control_subsys_t pclk_subsys;
	struct reset_dt_spec reset_core;
	struct reset_dt_spec reset_apb;
	void (*irq_config_func)(const struct device *dev);
};

struct rkcanfd_data {
	struct can_driver_data common;
	mm_reg_t regs;
	struct k_mutex mod_lock;
	struct k_spinlock tx_lock;
	struct k_sem tx_free;
	struct can_timing timing;
	struct rkcanfd_tx_frame tx[RKCANFD_TX_MAILBOXES];
	uint32_t tx_head;
	uint32_t tx_tail;
	enum can_state state;
	atomic_t rx_allocs;
	struct rkcanfd_filter filters[CONFIG_CAN_ROCKCHIP_CANFD_MAX_FILTERS];
};

static inline uint32_t rkcanfd_read(const struct device *dev, uint32_t reg)
{
	const struct rkcanfd_data *data = dev->data;

	return sys_read32(data->regs + reg);
}

static inline void rkcanfd_write(const struct device *dev, uint32_t reg, uint32_t val)
{
	const struct rkcanfd_data *data = dev->data;

	sys_write32(val, data->regs + reg);
}

static bool rkcanfd_frame_equal(const struct can_frame *a, const struct can_frame *b)
{
	const uint8_t flags = CAN_FRAME_IDE | CAN_FRAME_RTR;

	if (a->id != b->id || a->dlc != b->dlc || (a->flags & flags) != (b->flags & flags)) {
		return false;
	}

	if ((a->flags & CAN_FRAME_RTR) != 0U) {
		return true;
	}

	return memcmp(a->data, b->data, a->dlc) == 0;
}

/*
 * The controller prescaler is "2 * (register + 1)", i.e. it can only represent
 * even prescalers. The generic CAN timing calculation does not know about that
 * restriction and may return an odd prescaler. When it does, re-derive the
 * timing from the same bit time using the smallest even prescaler that fits.
 */
static bool rkcanfd_timing_from_bit_time(uint32_t bit_time, uint16_t sample_point,
					 uint16_t *prescaler, uint32_t *tseg1, uint32_t *tseg2)
{
	uint16_t p;

	for (p = RKCANFD_PRESCALER_MIN; p <= RKCANFD_PRESCALER_MAX; p += 2U) {
		uint32_t tq;
		uint32_t phase_seg2;
		uint32_t phase_seg1;

		if ((bit_time % p) != 0U) {
			continue;
		}

		tq = bit_time / p;
		if (tq < 4U || tq > RKCANFD_TQ_MAX) {
			continue;
		}

		phase_seg2 = tq - (tq * sample_point) / 1000U;
		phase_seg2 = CLAMP(phase_seg2, 1U, RKCANFD_TSEG2_MAX);
		phase_seg1 = tq - 1U - phase_seg2;

		if (phase_seg1 > RKCANFD_TSEG1_MAX) {
			phase_seg1 = RKCANFD_TSEG1_MAX;
			phase_seg2 = tq - 1U - phase_seg1;

			if (phase_seg2 > RKCANFD_TSEG2_MAX) {
				continue;
			}
		}

		if (phase_seg1 < 1U || phase_seg2 < 1U) {
			continue;
		}

		*prescaler = p;
		*tseg1 = phase_seg1;
		*tseg2 = phase_seg2;

		return true;
	}

	return false;
}

static int rkcanfd_set_timing(const struct device *dev, const struct can_timing *timing)
{
	struct rkcanfd_data *data = dev->data;
	uint32_t tseg1 = timing->prop_seg + timing->phase_seg1;
	uint32_t tseg2 = timing->phase_seg2;
	uint32_t prescaler = timing->prescaler;
	uint32_t sjw = timing->sjw;
	uint32_t reg;

	if (data->common.started) {
		return -EBUSY;
	}

	if (prescaler < RKCANFD_PRESCALER_MIN || prescaler > RKCANFD_PRESCALER_MAX ||
	    tseg1 < 1U || tseg1 > RKCANFD_TSEG1_MAX || tseg2 < 1U ||
	    tseg2 > RKCANFD_TSEG2_MAX || sjw < 1U || sjw > RKCANFD_SJW_MAX) {
		return -ENOTSUP;
	}

	if ((prescaler & 1U) != 0U) {
		uint32_t bit_time = prescaler * (1U + tseg1 + tseg2);
		uint16_t sample_point = ((1U + tseg1) * 1000U) / (1U + tseg1 + tseg2);
		uint16_t even_prescaler;

		if (!rkcanfd_timing_from_bit_time(bit_time, sample_point, &even_prescaler, &tseg1,
						  &tseg2)) {
			return -ENOTSUP;
		}

		prescaler = even_prescaler;
		sjw = MIN(sjw, MIN(tseg1, tseg2));
	}

	if (sjw > tseg1 || sjw > tseg2) {
		return -ENOTSUP;
	}

	reg = FIELD_PREP(RKCANFD_FD_BT_SJW, sjw - 1U) |
	      FIELD_PREP(RKCANFD_FD_BT_BRP, (prescaler / 2U) - 1U) |
	      FIELD_PREP(RKCANFD_FD_BT_TSEG2, tseg2 - 1U) |
	      FIELD_PREP(RKCANFD_FD_BT_TSEG1, tseg1 - 1U);

	rkcanfd_write(dev, RKCANFD_FD_NOMINAL_BITTIMING, reg);

	data->timing.sjw = (uint16_t)sjw;
	data->timing.prop_seg = 0U;
	data->timing.phase_seg1 = (uint16_t)tseg1;
	data->timing.phase_seg2 = (uint16_t)tseg2;
	data->timing.prescaler = (uint16_t)prescaler;

	return 0;
}

static int rkcanfd_get_core_clock(const struct device *dev, uint32_t *rate)
{
	const struct rkcanfd_config *config = dev->config;

	return clock_control_get_rate(config->clock_dev, config->clock_subsys, rate);
}

static int rkcanfd_get_capabilities(const struct device *dev, can_mode_t *cap)
{
	ARG_UNUSED(dev);

	*cap = CAN_MODE_NORMAL | CAN_MODE_LOOPBACK | CAN_MODE_LISTENONLY;

	return 0;
}

static int rkcanfd_get_max_filters(const struct device *dev, bool ide)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(ide);

	return CONFIG_CAN_ROCKCHIP_CANFD_MAX_FILTERS;
}

static int rkcanfd_add_rx_filter(const struct device *dev, can_rx_callback_t callback,
				 void *user_data, const struct can_filter *filter)
{
	struct rkcanfd_data *data = dev->data;
	int filter_id = -ENOSPC;

	if ((filter->flags & ~CAN_FILTER_IDE) != 0U) {
		LOG_ERR("unsupported CAN filter flags 0x%02x", filter->flags);
		return -ENOTSUP;
	}

	for (int i = 0; i < ARRAY_SIZE(data->filters); i++) {
		if (!atomic_test_and_set_bit(&data->rx_allocs, i)) {
			filter_id = i;
			break;
		}
	}

	if (filter_id >= 0) {
		data->filters[filter_id].filter = *filter;
		data->filters[filter_id].callback = callback;
		data->filters[filter_id].user_data = user_data;
	}

	return filter_id;
}

static void rkcanfd_remove_rx_filter(const struct device *dev, int filter_id)
{
	struct rkcanfd_data *data = dev->data;

	if (filter_id < 0 || filter_id >= (int)ARRAY_SIZE(data->filters)) {
		LOG_ERR("filter ID %d out of bounds", filter_id);
		return;
	}

	if (atomic_test_and_clear_bit(&data->rx_allocs, filter_id)) {
		data->filters[filter_id].callback = NULL;
		data->filters[filter_id].user_data = NULL;
		data->filters[filter_id].filter = (struct can_filter){0};
	}
}

static void rkcanfd_write_tx_frame(const struct device *dev, uint32_t slot)
{
	struct rkcanfd_data *data = dev->data;
	const struct can_frame *frame = &data->tx[slot].frame;
	uint32_t frameinfo = 0U;
	uint32_t id;
	uint32_t words[RKCANFD_TX_DATA_WORDS] = {0};

	if ((frame->flags & CAN_FRAME_IDE) != 0U) {
		frameinfo |= RKCANFD_FRAMEINFO_EFF;
		id = frame->id & RKCANFD_FD_ID_EFF;
	} else {
		id = frame->id & RKCANFD_FD_ID_SFF;
	}

	if ((frame->flags & CAN_FRAME_RTR) != 0U) {
		frameinfo |= RKCANFD_FRAMEINFO_RTR;
	}

	frameinfo |= FIELD_PREP(RKCANFD_FRAMEINFO_DLC, frame->dlc);

	if ((frame->flags & CAN_FRAME_RTR) == 0U) {
		memcpy(words, frame->data, frame->dlc);
	}

	rkcanfd_write(dev, RKCANFD_FD_TXFRAMEINFO, frameinfo);
	rkcanfd_write(dev, RKCANFD_FD_TXID, id);

	for (uint32_t i = 0U; i < ARRAY_SIZE(words); i++) {
		rkcanfd_write(dev, RKCANFD_FD_TXDATA(i), words[i]);
	}

	rkcanfd_write(dev, RKCANFD_CMD, RKCANFD_CMD_TX(slot));
}

static int rkcanfd_send(const struct device *dev, const struct can_frame *frame,
			k_timeout_t timeout, can_tx_callback_t callback, void *user_data)
{
	struct rkcanfd_data *data = dev->data;
	k_spinlock_key_t key;
	uint32_t slot;

	if (frame->dlc > CAN_MAX_DLC) {
		LOG_ERR("TX frame DLC %u exceeds maximum (%d)", frame->dlc, CAN_MAX_DLC);
		return -EINVAL;
	}

	if ((frame->flags & ~(CAN_FRAME_IDE | CAN_FRAME_RTR)) != 0U) {
		LOG_ERR("unsupported CAN frame flags 0x%02x", frame->flags);
		return -ENOTSUP;
	}

	if (!data->common.started) {
		return -ENETDOWN;
	}

	if (data->state == CAN_STATE_BUS_OFF) {
		return -ENETUNREACH;
	}

	if (k_sem_take(&data->tx_free, timeout) != 0) {
		return -EAGAIN;
	}

	key = k_spin_lock(&data->tx_lock);

	slot = data->tx_head % RKCANFD_TX_MAILBOXES;
	data->tx[slot].frame = *frame;
	data->tx[slot].callback = callback;
	data->tx[slot].user_data = user_data;
	data->tx_head++;

	rkcanfd_write_tx_frame(dev, slot);

	k_spin_unlock(&data->tx_lock, key);

	return 0;
}

static bool rkcanfd_handle_tx_done(const struct device *dev, const struct can_frame *frame)
{
	struct rkcanfd_data *data = dev->data;
	can_tx_callback_t callback = NULL;
	k_spinlock_key_t key;
	void *user_data = NULL;
	bool done = false;

	key = k_spin_lock(&data->tx_lock);

	if (data->tx_head != data->tx_tail) {
		uint32_t slot = data->tx_tail % RKCANFD_TX_MAILBOXES;
		struct rkcanfd_tx_frame *tx = &data->tx[slot];

		if (rkcanfd_frame_equal(&tx->frame, frame)) {
			callback = tx->callback;
			user_data = tx->user_data;
			tx->callback = NULL;
			data->tx_tail++;
			done = true;
		}
	}

	k_spin_unlock(&data->tx_lock, key);

	if (done) {
		k_sem_give(&data->tx_free);

		if (callback != NULL) {
			callback(dev, 0, user_data);
		}
	}

	return done;
}

/*
 * Extended frames may be transmitted as standard frames. Detect the mangled
 * frame and retry the pending extended frame.
 */
static bool rkcanfd_handle_tx_retry(const struct device *dev, const struct can_frame *frame)
{
	struct rkcanfd_data *data = dev->data;
	k_spinlock_key_t key;
	bool retried = false;

	key = k_spin_lock(&data->tx_lock);

	if (data->tx_head != data->tx_tail) {
		uint32_t slot = data->tx_tail % RKCANFD_TX_MAILBOXES;
		const struct can_frame *pending = &data->tx[slot].frame;

		if ((pending->flags & CAN_FRAME_IDE) != 0U &&
		    (frame->flags & CAN_FRAME_IDE) == 0U &&
		    (pending->id & RKCANFD_FD_ID_SFF) == frame->id &&
		    pending->dlc == frame->dlc &&
		    ((pending->flags ^ frame->flags) & CAN_FRAME_RTR) == 0U &&
		    (((pending->flags & CAN_FRAME_RTR) != 0U) ||
		     memcmp(pending->data, frame->data, pending->dlc) == 0)) {
			rkcanfd_write_tx_frame(dev, slot);
			retried = true;
		}
	}

	k_spin_unlock(&data->tx_lock, key);

	return retried;
}

static enum can_state rkcanfd_get_hw_state(const struct device *dev,
					   struct can_bus_err_cnt *err_cnt)
{
	uint32_t state = rkcanfd_read(dev, RKCANFD_STATE);
	uint32_t rx_err = FIELD_GET(RKCANFD_RXERRORCNT_MASK, rkcanfd_read(dev, RKCANFD_RXERRORCNT));
	uint32_t tx_err = FIELD_GET(RKCANFD_TXERRORCNT_MASK, rkcanfd_read(dev, RKCANFD_TXERRORCNT));

	if (err_cnt != NULL) {
		err_cnt->rx_err_cnt = (uint8_t)rx_err;
		err_cnt->tx_err_cnt = (uint8_t)tx_err;
	}

	if ((state & RKCANFD_STATE_BUS_OFF) != 0U) {
		return CAN_STATE_BUS_OFF;
	}

	if (rx_err > 127U || tx_err > 127U) {
		return CAN_STATE_ERROR_PASSIVE;
	}

	if ((state & RKCANFD_STATE_ERROR_WARNING) != 0U) {
		return CAN_STATE_ERROR_WARNING;
	}

	return CAN_STATE_ERROR_ACTIVE;
}

static int rkcanfd_get_state(const struct device *dev, enum can_state *state,
			     struct can_bus_err_cnt *err_cnt)
{
	struct rkcanfd_data *data = dev->data;

	if (state != NULL) {
		if (!data->common.started) {
			*state = CAN_STATE_STOPPED;
		} else {
			*state = data->state;
		}
	}

	if (err_cnt != NULL) {
		(void)rkcanfd_get_hw_state(dev, err_cnt);
	}

	return 0;
}

static void rkcanfd_tx_abort(const struct device *dev, int status)
{
	struct rkcanfd_data *data = dev->data;

	for (;;) {
		can_tx_callback_t callback;
		k_spinlock_key_t key;
		void *user_data;
		uint32_t slot;

		key = k_spin_lock(&data->tx_lock);

		if (data->tx_head == data->tx_tail) {
			k_spin_unlock(&data->tx_lock, key);
			break;
		}

		slot = data->tx_tail % RKCANFD_TX_MAILBOXES;
		callback = data->tx[slot].callback;
		user_data = data->tx[slot].user_data;
		data->tx[slot].callback = NULL;
		data->tx_tail++;

		k_spin_unlock(&data->tx_lock, key);

		if (callback != NULL) {
			callback(dev, status, user_data);
		}

		k_sem_give(&data->tx_free);
	}
}

static bool rkcanfd_read_rx_frame(const struct device *dev, struct can_frame *frame, bool *is_fd)
{
	uint32_t frameinfo = rkcanfd_read(dev, RKCANFD_RX_FIFO_RDATA);
	uint32_t id = rkcanfd_read(dev, RKCANFD_RX_FIFO_RDATA);
	uint32_t timestamp = rkcanfd_read(dev, RKCANFD_RX_FIFO_RDATA);
	uint32_t words[RKCANFD_RX_DATA_WORDS];
	uint32_t dlc;

	for (uint32_t i = 0U; i < ARRAY_SIZE(words); i++) {
		words[i] = rkcanfd_read(dev, RKCANFD_RX_FIFO_RDATA);
	}

	memset(frame, 0, sizeof(*frame));
	*is_fd = false;

	/* Erratum 5: an empty FIFO element repeats the header word. */
	if (frameinfo == id && frameinfo == timestamp) {
		return false;
	}

	if ((frameinfo & RKCANFD_FRAMEINFO_FDF) != 0U) {
		*is_fd = true;
		return true;
	}

	if ((frameinfo & RKCANFD_FRAMEINFO_EFF) != 0U) {
		frame->flags |= CAN_FRAME_IDE;
		frame->id = FIELD_GET(RKCANFD_FD_ID_EFF, id);
	} else {
		frame->id = FIELD_GET(RKCANFD_FD_ID_SFF, id);
	}

	if ((frameinfo & RKCANFD_FRAMEINFO_RTR) != 0U) {
		frame->flags |= CAN_FRAME_RTR;
	}

	dlc = FIELD_GET(RKCANFD_FRAMEINFO_DLC, frameinfo);
	frame->dlc = (uint8_t)MIN(dlc, (uint32_t)CAN_MAX_DLC);

	if ((frame->flags & CAN_FRAME_RTR) == 0U) {
		memcpy(frame->data, words, frame->dlc);
	}

	return true;
}

static void rkcanfd_rx(const struct device *dev)
{
	struct rkcanfd_data *data = dev->data;

	while (FIELD_GET(RKCANFD_RX_FIFO_CNT, rkcanfd_read(dev, RKCANFD_RX_FIFO_CTRL)) != 0U) {
		struct can_frame frame;
		bool is_fd = false;

		if (!rkcanfd_read_rx_frame(dev, &frame, &is_fd)) {
			break;
		}

		if (is_fd) {
			/* CAN FD is out of scope; drop the frame. */
			continue;
		}

		if (rkcanfd_handle_tx_done(dev, &frame)) {
			if ((data->common.mode & CAN_MODE_LOOPBACK) == 0U) {
				continue;
			}
		} else if (rkcanfd_handle_tx_retry(dev, &frame)) {
			continue;
		}

#ifndef CONFIG_CAN_ACCEPT_RTR
		if ((frame.flags & CAN_FRAME_RTR) != 0U) {
			continue;
		}
#endif /* CONFIG_CAN_ACCEPT_RTR */

		for (int i = 0; i < ARRAY_SIZE(data->filters); i++) {
			can_rx_callback_t callback;

			if (!atomic_test_bit(&data->rx_allocs, i)) {
				continue;
			}

			if (!can_frame_matches_filter(&frame, &data->filters[i].filter)) {
				continue;
			}

			callback = data->filters[i].callback;
			if (callback != NULL) {
				callback(dev, &frame, data->filters[i].user_data);
			}
		}
	}
}

static void rkcanfd_isr(const struct device *dev)
{
	struct rkcanfd_data *data = dev->data;
	enum can_state prev_state = data->state;
	struct can_bus_err_cnt err_cnt;
	uint32_t pending;

	pending = rkcanfd_read(dev, RKCANFD_INT);
	rkcanfd_write(dev, RKCANFD_INT, pending);

	if ((pending & RKCANFD_INT_RX_FINISH) != 0U) {
		rkcanfd_rx(dev);
	}

	if ((pending & RKCANFD_INT_RX_FIFO_OVERFLOW) != 0U) {
		CAN_STATS_RX_OVERRUN_INC(dev);
	}

	if ((pending & RKCANFD_INT_BUS_OFF) != 0U) {
		data->state = CAN_STATE_BUS_OFF;
		rkcanfd_tx_abort(dev, -ENETUNREACH);
	} else if ((pending & (RKCANFD_INT_ERROR_WARNING | RKCANFD_INT_PASSIVE_ERROR |
			       RKCANFD_INT_BUS_OFF_RECOVERY)) != 0U) {
		data->state = rkcanfd_get_hw_state(dev, &err_cnt);
	}

	if (prev_state != data->state) {
		(void)rkcanfd_get_hw_state(dev, &err_cnt);
		can_fire_state_change_callbacks(dev, data->state, err_cnt);
	}
}

static int rkcanfd_start(const struct device *dev)
{
	const struct rkcanfd_config *config = dev->config;
	struct rkcanfd_data *data = dev->data;
	uint32_t mode;
	int err;

	if (data->common.started) {
		return -EALREADY;
	}

	if (config->common.phy != NULL) {
		err = can_transceiver_enable(config->common.phy, data->common.mode);
		if (err != 0) {
			LOG_ERR("failed to enable CAN transceiver (err %d)", err);
			return err;
		}
	}

	err = reset_line_assert_dt(&config->reset_core);
	if (err != 0) {
		goto phy_disable;
	}

	k_busy_wait(2);

	err = reset_line_deassert_dt(&config->reset_core);
	if (err != 0) {
		goto phy_disable;
	}

	rkcanfd_write(dev, RKCANFD_MODE, 0U);

	/* Accept all frames in hardware and filter in software. */
	rkcanfd_write(dev, RKCANFD_IDCODE, 0U);
	rkcanfd_write(dev, RKCANFD_IDMASK, RKCANFD_IDMASK_ALL);

	rkcanfd_write(dev, RKCANFD_RX_FIFO_CTRL,
		      rkcanfd_read(dev, RKCANFD_RX_FIFO_CTRL) | RKCANFD_RX_FIFO_ENABLE);

	rkcanfd_write(dev, RKCANFD_INT, RKCANFD_INT_ALL);
	rkcanfd_write(dev, RKCANFD_INT_MASK,
		      RKCANFD_INT_TIMESTAMP_OVF | RKCANFD_INT_TX_ARBIT_FAIL |
			      RKCANFD_INT_OVERLOAD | RKCANFD_INT_TX_FINISH | RKCANFD_INT_ERROR);

	data->tx_head = 0U;
	data->tx_tail = 0U;
	k_sem_reset(&data->tx_free);
	for (uint32_t i = 0U; i < RKCANFD_TX_MAILBOXES; i++) {
		k_sem_give(&data->tx_free);
	}

	/* The bit timing register is reset by the core reset above. */
	(void)rkcanfd_set_timing(dev, &data->timing);

	/*
	 * Keep the controller's FD framing mode enabled: the RX FIFO layout used
	 * here is the one the controller produces in that mode. CAN FD frames are
	 * dropped in software and never transmitted.
	 */
	mode = RKCANFD_MODE_CAN_FD | RKCANFD_MODE_AUTO_RETX | RKCANFD_MODE_COVER |
	       RKCANFD_MODE_RXSTX | RKCANFD_MODE_WORK;

	if ((data->common.mode & CAN_MODE_LOOPBACK) != 0U) {
		mode |= RKCANFD_MODE_LBACK | RKCANFD_MODE_SILENT | RKCANFD_MODE_SELF_TEST;
	} else if ((data->common.mode & CAN_MODE_LISTENONLY) != 0U) {
		mode |= RKCANFD_MODE_SILENT;
	}

	data->state = CAN_STATE_ERROR_ACTIVE;

	rkcanfd_write(dev, RKCANFD_MODE, mode);

	data->common.started = true;

	return 0;

phy_disable:
	if (config->common.phy != NULL) {
		(void)can_transceiver_disable(config->common.phy);
	}

	return err;
}

static int rkcanfd_stop(const struct device *dev)
{
	const struct rkcanfd_config *config = dev->config;
	struct rkcanfd_data *data = dev->data;
	int err = 0;

	if (!data->common.started) {
		return -EALREADY;
	}

	rkcanfd_write(dev, RKCANFD_INT_MASK, RKCANFD_INT_ALL);
	rkcanfd_write(dev, RKCANFD_MODE, 0U);

	rkcanfd_tx_abort(dev, -ENETDOWN);

	if (config->common.phy != NULL) {
		err = can_transceiver_disable(config->common.phy);
		if (err != 0) {
			LOG_ERR("failed to disable CAN transceiver (err %d)", err);
		}
	}

	data->common.started = false;
	data->state = CAN_STATE_STOPPED;

	return err;
}

static int rkcanfd_set_mode(const struct device *dev, can_mode_t mode)
{
	struct rkcanfd_data *data = dev->data;
	can_mode_t supported = CAN_MODE_LOOPBACK | CAN_MODE_LISTENONLY;

	if ((mode & ~supported) != 0U) {
		LOG_ERR("unsupported mode: 0x%08x", mode);
		return -ENOTSUP;
	}

	if (data->common.started) {
		return -EBUSY;
	}

	k_mutex_lock(&data->mod_lock, K_FOREVER);
	data->common.mode = mode;
	k_mutex_unlock(&data->mod_lock);

	return 0;
}

static int rkcanfd_init(const struct device *dev)
{
	const struct rkcanfd_config *config = dev->config;
	struct rkcanfd_data *data = dev->data;
	struct can_timing timing = {0};
	int err;

	device_map(&data->regs, config->phys, config->size, K_MEM_CACHE_NONE);

	if (!device_is_ready(config->clock_dev)) {
		LOG_ERR_DEVICE_NOT_READY(config->clock_dev);
		return -ENODEV;
	}

	if (!device_is_ready(config->pclk_dev)) {
		LOG_ERR_DEVICE_NOT_READY(config->pclk_dev);
		return -ENODEV;
	}

	if (!device_is_ready(config->reset_core.dev)) {
		LOG_ERR_DEVICE_NOT_READY(config->reset_core.dev);
		return -ENODEV;
	}

	if (!device_is_ready(config->reset_apb.dev)) {
		LOG_ERR_DEVICE_NOT_READY(config->reset_apb.dev);
		return -ENODEV;
	}

	if (config->common.phy != NULL && !device_is_ready(config->common.phy)) {
		LOG_ERR_DEVICE_NOT_READY(config->common.phy);
		return -ENODEV;
	}

	sys_slist_init(&data->common.state_change_callbacks);
	k_mutex_init(&data->mod_lock);
	k_sem_init(&data->tx_free, RKCANFD_TX_MAILBOXES, RKCANFD_TX_MAILBOXES);

	data->state = CAN_STATE_STOPPED;
	data->common.mode = CAN_MODE_NORMAL;

	err = clock_control_on(config->pclk_dev, config->pclk_subsys);
	if (err != 0) {
		LOG_ERR("failed to enable CAN APB clock (err %d)", err);
		return err;
	}

	err = clock_control_on(config->clock_dev, config->clock_subsys);
	if (err != 0) {
		LOG_ERR("failed to enable CAN clock (err %d)", err);
		return err;
	}

	err = reset_line_assert_dt(&config->reset_apb);
	if (err != 0) {
		return err;
	}

	err = reset_line_assert_dt(&config->reset_core);
	if (err != 0) {
		return err;
	}

	k_busy_wait(2);

	err = reset_line_deassert_dt(&config->reset_core);
	if (err != 0) {
		return err;
	}

	err = reset_line_deassert_dt(&config->reset_apb);
	if (err != 0) {
		return err;
	}

	rkcanfd_write(dev, RKCANFD_MODE, 0U);

	err = can_calc_timing(dev, &timing, config->common.bitrate, config->common.sample_point);
	if (err < 0) {
		LOG_ERR("bitrate/sample point cannot be met (err %d)", err);
		return err;
	}

	err = can_set_timing(dev, &timing);
	if (err != 0) {
		LOG_ERR("timing parameters cannot be met (err %d)", err);
		return err;
	}

	config->irq_config_func(dev);

	return 0;
}

#define RKCANFD_INIT(inst)                                                                    \
	static void rkcanfd_irq_config_##inst(const struct device *dev)                       \
	{                                                                                     \
		ARG_UNUSED(dev);                                                              \
		IRQ_CONNECT(DT_INST_IRQN(inst), DT_INST_IRQ(inst, priority), rkcanfd_isr,     \
			    DEVICE_DT_INST_GET(inst), 0);                                     \
		irq_enable(DT_INST_IRQN(inst));                                               \
	}                                                                                     \
                                                                                              \
	static const struct rkcanfd_config rkcanfd_config_##inst = {                          \
		.common = CAN_DT_DRIVER_CONFIG_INST_GET(inst, 10000, 1000000),                \
		.phys = DT_INST_REG_ADDR(inst),                                               \
		.size = DT_INST_REG_SIZE(inst),                                               \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR_BY_NAME(inst, baud)),          \
		.clock_subsys = (clock_control_subsys_t)                                      \
			DT_INST_CLOCKS_CELL_BY_NAME(inst, baud, clkid),                       \
		.pclk_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR_BY_NAME(inst, pclk)),           \
		.pclk_subsys = (clock_control_subsys_t)                                       \
			DT_INST_CLOCKS_CELL_BY_NAME(inst, pclk, clkid),                       \
		.reset_core = RESET_DT_SPEC_GET_BY_IDX(DT_DRV_INST(inst), 0),                 \
		.reset_apb = RESET_DT_SPEC_GET_BY_IDX(DT_DRV_INST(inst), 1),                  \
		.irq_config_func = rkcanfd_irq_config_##inst,                                 \
	};                                                                                    \
                                                                                              \
	static struct rkcanfd_data rkcanfd_data_##inst;                                       \
                                                                                              \
	static DEVICE_API(can, rkcanfd_api_##inst) = {                                        \
		.get_capabilities = rkcanfd_get_capabilities,                                 \
		.start = rkcanfd_start,                                                       \
		.stop = rkcanfd_stop,                                                         \
		.set_mode = rkcanfd_set_mode,                                                 \
		.set_timing = rkcanfd_set_timing,                                             \
		.send = rkcanfd_send,                                                         \
		.add_rx_filter = rkcanfd_add_rx_filter,                                       \
		.remove_rx_filter = rkcanfd_remove_rx_filter,                                 \
		.get_state = rkcanfd_get_state,                                               \
		.get_core_clock = rkcanfd_get_core_clock,                                     \
		.get_max_filters = rkcanfd_get_max_filters,                                   \
		.timing_min = {                                                               \
			.sjw = 1,                                                             \
			.prop_seg = 0,                                                        \
			.phase_seg1 = 1,                                                      \
			.phase_seg2 = 1,                                                      \
			.prescaler = RKCANFD_PRESCALER_MIN,                                   \
		},                                                                            \
		.timing_max = {                                                               \
			.sjw = RKCANFD_SJW_MAX,                                               \
			.prop_seg = 0,                                                        \
			.phase_seg1 = RKCANFD_TSEG1_MAX,                                      \
			.phase_seg2 = RKCANFD_TSEG2_MAX,                                      \
			.prescaler = RKCANFD_PRESCALER_MAX,                                   \
		},                                                                            \
	};                                                                                    \
                                                                                              \
	DEVICE_DT_INST_DEFINE(inst, rkcanfd_init, NULL, &rkcanfd_data_##inst,                 \
			      &rkcanfd_config_##inst, POST_KERNEL,                            \
			      CONFIG_CAN_INIT_PRIORITY, &rkcanfd_api_##inst);

DT_INST_FOREACH_STATUS_OKAY(RKCANFD_INIT)
