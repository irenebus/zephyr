/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Rockchip CAN Driver Tests - Mode and Timing Configuration Tests
 */

#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "common.h"

/**
 * @brief Test setting CAN_MODE_NORMAL
 */
ZTEST(rockchip_can_mode_timing, test_set_mode_normal)
{
	int ret;

	ret = can_set_mode(can_dev, CAN_MODE_NORMAL);
	zassert_equal(ret, 0, "Failed to set NORMAL mode");
}

/**
 * @brief Test setting CAN_MODE_LOOPBACK
 */
ZTEST(rockchip_can_mode_timing, test_set_mode_loopback)
{
	int ret;

	can_set_mode(can_dev, CAN_MODE_NORMAL);
	ret = can_set_mode(can_dev, CAN_MODE_LOOPBACK);
	zassert_equal(ret, 0, "Failed to set LOOPBACK mode");
}

/**
 * @brief Test setting CAN_MODE_LISTENONLY
 */
ZTEST(rockchip_can_mode_timing, test_set_mode_listenonly)
{
	int ret;

	can_set_mode(can_dev, CAN_MODE_NORMAL);
	ret = can_set_mode(can_dev, CAN_MODE_LISTENONLY);
	zassert_equal(ret, 0, "Failed to set LISTENONLY mode");
}

/**
 * @brief Test setting CAN_MODE_ONE_SHOT
 */
ZTEST(rockchip_can_mode_timing, test_set_mode_one_shot)
{
	int ret;

	can_set_mode(can_dev, CAN_MODE_NORMAL);
	ret = can_set_mode(can_dev, CAN_MODE_ONE_SHOT);
	zassert_equal(ret, 0, "Failed to set ONE_SHOT mode");
}

/**
 * @brief Test setting mode after start returns EBUSY
 */
ZTEST(rockchip_can_mode_timing, test_set_mode_after_start_fails)
{
	int ret;

	can_set_mode(can_dev, CAN_MODE_NORMAL);
	ret = can_start(can_dev);
	zassert_equal(ret, 0, "Failed to start CAN device");

	ret = can_set_mode(can_dev, CAN_MODE_LOOPBACK);
	zassert_equal(ret, -EBUSY, "Should not allow mode change after start");

	can_stop(can_dev);
}

/**
 * @brief Test basic timing configuration
 */
ZTEST(rockchip_can_mode_timing, test_set_timing_valid)
{
	struct can_timing timing = {
		.sjw = 2,
		.prop_seg = 0,
		.phase_seg1 = 8,
		.phase_seg2 = 4,
		.prescaler = 10,
	};
	int ret;

	ret = can_set_timing(can_dev, &timing);
	zassert_equal(ret, 0, "Failed to set valid timing");
}

/**
 * @brief Test timing boundary values - minimum
 */
ZTEST(rockchip_can_mode_timing, test_set_timing_min_values)
{
	struct can_timing timing = {
		.sjw = 1,
		.prop_seg = 0,
		.phase_seg1 = 1,
		.phase_seg2 = 1,
		.prescaler = 1,
	};
	int ret;

	ret = can_set_timing(can_dev, &timing);
	zassert_equal(ret, 0, "Failed to set minimum timing values");
}

/**
 * @brief Test timing boundary values - maximum
 */
ZTEST(rockchip_can_mode_timing, test_set_timing_max_values)
{
	struct can_timing timing = {
		.sjw = 4,
		.prop_seg = 0,
		.phase_seg1 = 16,
		.phase_seg2 = 8,
		.prescaler = 64,
	};
	int ret;

	ret = can_set_timing(can_dev, &timing);
	zassert_equal(ret, 0, "Failed to set maximum timing values");
}

/**
 * @brief Test timing with invalid SJW (too low)
 */
ZTEST(rockchip_can_mode_timing, test_set_timing_sjw_too_low)
{
	struct can_timing timing = {
		.sjw = 0,
		.prop_seg = 0,
		.phase_seg1 = 8,
		.phase_seg2 = 4,
		.prescaler = 10,
	};
	int ret;

	ret = can_set_timing(can_dev, &timing);
	zassert_equal(ret, -EINVAL, "Should reject SJW < 1");
}

/**
 * @brief Test timing with invalid SJW (too high)
 */
ZTEST(rockchip_can_mode_timing, test_set_timing_sjw_too_high)
{
	struct can_timing timing = {
		.sjw = 5,
		.prop_seg = 0,
		.phase_seg1 = 8,
		.phase_seg2 = 4,
		.prescaler = 10,
	};
	int ret;

	ret = can_set_timing(can_dev, &timing);
	zassert_equal(ret, -EINVAL, "Should reject SJW > 4");
}

/**
 * @brief Test timing with invalid prescaler (too low)
 */
ZTEST(rockchip_can_mode_timing, test_set_timing_prescaler_too_low)
{
	struct can_timing timing = {
		.sjw = 2,
		.prop_seg = 0,
		.phase_seg1 = 8,
		.phase_seg2 = 4,
		.prescaler = 0,
	};
	int ret;

	ret = can_set_timing(can_dev, &timing);
	zassert_equal(ret, -EINVAL, "Should reject prescaler < 1");
}

/**
 * @brief Test timing with invalid prescaler (too high)
 */
ZTEST(rockchip_can_mode_timing, test_set_timing_prescaler_too_high)
{
	struct can_timing timing = {
		.sjw = 2,
		.prop_seg = 0,
		.phase_seg1 = 8,
		.phase_seg2 = 4,
		.prescaler = 65,
	};
	int ret;

	ret = can_set_timing(can_dev, &timing);
	zassert_equal(ret, -EINVAL, "Should reject prescaler > 64");
}

/**
 * @brief Test timing with non-zero prop_seg (not allowed)
 */
ZTEST(rockchip_can_mode_timing, test_set_timing_prop_seg_nonzero)
{
	struct can_timing timing = {
		.sjw = 2,
		.prop_seg = 1,  /* Should be 0 for Rockchip */
		.phase_seg1 = 8,
		.phase_seg2 = 4,
		.prescaler = 10,
	};
	int ret;

	ret = can_set_timing(can_dev, &timing);
	zassert_equal(ret, -EINVAL, "Should reject prop_seg != 0");
}

/**
 * @brief Test timing after start returns EBUSY
 */
ZTEST(rockchip_can_mode_timing, test_set_timing_after_start_fails)
{
	struct can_timing timing = {
		.sjw = 2,
		.prop_seg = 0,
		.phase_seg1 = 8,
		.phase_seg2 = 4,
		.prescaler = 10,
	};
	int ret;

	ret = can_start(can_dev);
	zassert_equal(ret, 0, "Failed to start CAN device");

	ret = can_set_timing(can_dev, &timing);
	zassert_equal(ret, -EBUSY, "Should not allow timing change after start");

	can_stop(can_dev);
}

/**
 * @brief Test setting timing with NULL pointer
 */
ZTEST(rockchip_can_mode_timing, test_set_timing_null_pointer)
{
	int ret;

	ret = can_set_timing(can_dev, NULL);
	zassert_equal(ret, -EINVAL, "Should reject NULL timing pointer");
}

/**
 * @brief Test suite setup
 */
static void *rockchip_can_mode_timing_setup(void)
{
	test_setup();
	return NULL;
}

/**
 * @brief Test suite teardown
 */
static void rockchip_can_mode_timing_teardown(void *fixture)
{
	ARG_UNUSED(fixture);
	test_teardown();
}

ZTEST_SUITE(rockchip_can_mode_timing, NULL, rockchip_can_mode_timing_setup, NULL,
	    NULL, rockchip_can_mode_timing_teardown);
