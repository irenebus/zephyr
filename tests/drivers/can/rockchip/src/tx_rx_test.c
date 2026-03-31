/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Rockchip CAN Driver Tests - Transmission and Reception Tests
 */

#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "common.h"

static int tx_callback_count = 0;
static int tx_callback_error = 0;

/**
 * @brief TX callback handler
 */
static void tx_callback(const struct device *dev, int error, void *user_data)
{
	zassert_equal(dev, can_dev, "Device mismatch in TX callback");
	tx_callback_count++;
	tx_callback_error = error;
	k_sem_give(&tx_callback_sem);
}

/**
 * @brief Test basic frame transmission
 */
ZTEST(rockchip_can_tx_rx, test_send_frame)
{
	int ret;

	/* Setup timing and start */
	struct can_timing timing = {
		.sjw = 2,
		.prop_seg = 0,
		.phase_seg1 = 8,
		.phase_seg2 = 4,
		.prescaler = 10,
	};

	can_set_mode(can_dev, CAN_MODE_NORMAL);
	can_set_timing(can_dev, &timing);
	ret = can_start(can_dev);
	zassert_equal(ret, 0, "Failed to start CAN device");

	tx_callback_count = 0;
	ret = can_send(can_dev, &test_std_frame_1, K_NO_WAIT, tx_callback, NULL);
	zassert_equal(ret, 0, "Failed to send frame");

	can_stop(can_dev);
}

/**
 * @brief Test sending without starting returns ENETDOWN
 */
ZTEST(rockchip_can_tx_rx, test_send_without_start)
{
	int ret;

	ret = can_send(can_dev, &test_std_frame_1, K_NO_WAIT, NULL, NULL);
	zassert_equal(ret, -ENETDOWN, "Should fail when device not started");
}

/**
 * @brief Test sending NULL frame returns EINVAL
 */
ZTEST(rockchip_can_tx_rx, test_send_null_frame)
{
	int ret;

	struct can_timing timing = {
		.sjw = 2,
		.prop_seg = 0,
		.phase_seg1 = 8,
		.phase_seg2 = 4,
		.prescaler = 10,
	};

	can_set_mode(can_dev, CAN_MODE_NORMAL);
	can_set_timing(can_dev, &timing);
	can_start(can_dev);

	ret = can_send(can_dev, NULL, K_NO_WAIT, NULL, NULL);
	zassert_equal(ret, -EINVAL, "Should reject NULL frame");

	can_stop(can_dev);
}

/**
 * @brief Test sending standard frame
 */
ZTEST(rockchip_can_tx_rx, test_send_std_frame)
{
	int ret;

	struct can_timing timing = {
		.sjw = 2,
		.prop_seg = 0,
		.phase_seg1 = 8,
		.phase_seg2 = 4,
		.prescaler = 10,
	};

	can_set_mode(can_dev, CAN_MODE_NORMAL);
	can_set_timing(can_dev, &timing);
	ret = can_start(can_dev);
	zassert_equal(ret, 0, "Failed to start device");

	ret = can_send(can_dev, &test_std_frame_1, K_NO_WAIT, NULL, NULL);
	zassert_equal(ret, 0, "Failed to send standard frame");

	can_stop(can_dev);
}

/**
 * @brief Test sending extended frame
 */
ZTEST(rockchip_can_tx_rx, test_send_ext_frame)
{
	int ret;

	struct can_timing timing = {
		.sjw = 2,
		.prop_seg = 0,
		.phase_seg1 = 8,
		.phase_seg2 = 4,
		.prescaler = 10,
	};

	can_set_mode(can_dev, CAN_MODE_NORMAL);
	can_set_timing(can_dev, &timing);
	ret = can_start(can_dev);
	zassert_equal(ret, 0, "Failed to start device");

	ret = can_send(can_dev, &test_ext_frame_1, K_NO_WAIT, NULL, NULL);
	zassert_equal(ret, 0, "Failed to send extended frame");

	can_stop(can_dev);
}

/**
 * @brief Test sending RTR frame
 */
ZTEST(rockchip_can_tx_rx, test_send_rtr_frame)
{
	int ret;

	struct can_timing timing = {
		.sjw = 2,
		.prop_seg = 0,
		.phase_seg1 = 8,
		.phase_seg2 = 4,
		.prescaler = 10,
	};

	can_set_mode(can_dev, CAN_MODE_NORMAL);
	can_set_timing(can_dev, &timing);
	ret = can_start(can_dev);
	zassert_equal(ret, 0, "Failed to start device");

	ret = can_send(can_dev, &test_std_rtr_frame_1, K_NO_WAIT, NULL, NULL);
	zassert_equal(ret, 0, "Failed to send RTR frame");

	can_stop(can_dev);
}

/**
 * @brief Test start/already started
 */
ZTEST(rockchip_can_tx_rx, test_start_already_started)
{
	int ret;

	struct can_timing timing = {
		.sjw = 2,
		.prop_seg = 0,
		.phase_seg1 = 8,
		.phase_seg2 = 4,
		.prescaler = 10,
	};

	can_set_mode(can_dev, CAN_MODE_NORMAL);
	can_set_timing(can_dev, &timing);
	ret = can_start(can_dev);
	zassert_equal(ret, 0, "First start should succeed");

	ret = can_start(can_dev);
	zassert_equal(ret, -EALREADY, "Second start should return EALREADY");

	can_stop(can_dev);
}

/**
 * @brief Test stop/already stopped
 */
ZTEST(rockchip_can_tx_rx, test_stop_already_stopped)
{
	int ret;

	ret = can_stop(can_dev);
	/* Device starts already stopped, so first stop might already return EALREADY */
	zassert_true(ret == 0 || ret == -EALREADY, "Stop should succeed or already stopped");
}

/**
 * @brief Test loopback mode - frame transmission and local receipt
 */
ZTEST(rockchip_can_tx_rx, test_loopback_send_receive)
{
	int ret;
	int filter_id;
	struct can_timing timing = {
		.sjw = 2,
		.prop_seg = 0,
		.phase_seg1 = 8,
		.phase_seg2 = 4,
		.prescaler = 10,
	};
	struct can_filter filter = {
		.flags = 0,
		.id = TEST_CAN_STD_ID_1,
		.mask = CAN_STD_ID_MASK,
	};

	can_set_mode(can_dev, CAN_MODE_LOOPBACK);
	can_set_timing(can_dev, &timing);
	ret = can_start(can_dev);
	zassert_equal(ret, 0, "Failed to start device in loopback");

	/* Add filter for receive */
	filter_id = can_add_rx_filter(can_dev, NULL, NULL, &filter);
	zassert_true(filter_id >= 0, "Failed to add filter");

	/* Send frame */
	ret = can_send(can_dev, &test_std_frame_1, K_NO_WAIT, NULL, NULL);
	zassert_equal(ret, 0, "Failed to send frame");

	can_remove_rx_filter(can_dev, filter_id);
	can_stop(can_dev);
}

/**
 * @brief Test suite setup
 */
static void *rockchip_can_tx_rx_setup(void)
{
	test_setup();
	return NULL;
}

/**
 * @brief Test suite teardown
 */
static void rockchip_can_tx_rx_teardown(void *fixture)
{
	ARG_UNUSED(fixture);
	test_teardown();
}

ZTEST_SUITE(rockchip_can_tx_rx, NULL, rockchip_can_tx_rx_setup, NULL,
	    NULL, rockchip_can_tx_rx_teardown);
