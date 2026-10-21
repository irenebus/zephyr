/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Gate 4 test app: CAN shell plus a 1000-frame loopback burst command.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#define BURST_FRAMES 1000U
#define BURST_TIMEOUT_MS 5000

static const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
static atomic_t rx_frames;
static atomic_t tx_done;
static int filter_id = -1;

static void burst_rx_cb(const struct device *dev, struct can_frame *frame, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(frame);
	ARG_UNUSED(user_data);

	atomic_inc(&rx_frames);
}

static void burst_tx_cb(const struct device *dev, int error, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	if (error == 0) {
		atomic_inc(&tx_done);
	}
}

static int cmd_can_burst(const struct shell *sh, size_t argc, char **argv)
{
	struct can_frame frame = { .id = 0x123U, .dlc = 8U };
	int64_t deadline;
	uint32_t clock;
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!device_is_ready(can_dev)) {
		shell_error(sh, "can0 not ready");
		return -ENODEV;
	}

	err = can_get_core_clock(can_dev, &clock);
	if (err != 0) {
		shell_error(sh, "get core clock failed (err %d)", err);
		return err;
	}

	err = can_stop(can_dev);
	if (err != 0 && err != -EALREADY) {
		shell_error(sh, "stop failed (err %d)", err);
		return err;
	}

	err = can_set_mode(can_dev, CAN_MODE_LOOPBACK);
	if (err != 0) {
		shell_error(sh, "set loopback failed (err %d)", err);
		return err;
	}

	if (filter_id < 0) {
		const struct can_filter filter = { .flags = 0U, .id = 0U, .mask = 0U };

		filter_id = can_add_rx_filter(can_dev, burst_rx_cb, NULL, &filter);
		if (filter_id < 0) {
			shell_error(sh, "add filter failed (err %d)", filter_id);
			return filter_id;
		}
	}

	atomic_set(&rx_frames, 0);
	atomic_set(&tx_done, 0);

	err = can_start(can_dev);
	if (err != 0) {
		shell_error(sh, "start failed (err %d)", err);
		return err;
	}

	for (uint32_t i = 0U; i < BURST_FRAMES; i++) {
		frame.data[0] = (uint8_t)i;
		frame.data[1] = (uint8_t)(i >> 8);

		err = can_send(can_dev, &frame, K_MSEC(1000), burst_tx_cb, NULL);
		if (err != 0) {
			shell_error(sh, "send %u failed (err %d)", i, err);
			break;
		}
	}

	deadline = k_uptime_get() + BURST_TIMEOUT_MS;
	while ((atomic_get(&rx_frames) < BURST_FRAMES ||
		atomic_get(&tx_done) < BURST_FRAMES) &&
	       k_uptime_get() < deadline) {
		k_sleep(K_MSEC(10));
	}
	k_sleep(K_MSEC(100));

	shell_print(sh, "BURST: core_clock=%u Hz sent=%u tx_done=%u rx=%u expected=%u -> %s",
		    clock, BURST_FRAMES, (unsigned int)atomic_get(&tx_done),
		    (unsigned int)atomic_get(&rx_frames), BURST_FRAMES,
		    (atomic_get(&tx_done) == BURST_FRAMES &&
		     atomic_get(&rx_frames) == BURST_FRAMES) ? "PASS" : "FAIL");

	(void)can_stop(can_dev);

	return 0;
}

SHELL_CMD_ARG_REGISTER(can_burst, NULL, "Run a 1000-frame loopback burst",
		       cmd_can_burst, 1, 0);
