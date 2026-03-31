/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Rockchip CAN Driver Tests - Initialization Tests
 */

#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "common.h"

/**
 * @brief Test that device is ready
 */
ZTEST(rockchip_can_init, test_device_ready)
{
	zassert_true(device_is_ready(can_dev), "CAN device is not ready");
}

/**
 * @brief Test getting CAN capabilities
 */
ZTEST(rockchip_can_init, test_get_capabilities)
{
	can_mode_t capabilities = 0;
	int ret;

	ret = can_get_capabilities(can_dev, &capabilities);
	zassert_equal(ret, 0, "Failed to get capabilities");
	zassert_not_equal(capabilities, 0, "No capabilities reported");

	/* Rockchip should support at least NORMAL mode */
	zassert_true(capabilities & CAN_MODE_NORMAL, "NORMAL mode not supported");

	/* Check for expected optional modes */
	zassert_true(capabilities & CAN_MODE_LOOPBACK, "LOOPBACK mode not supported");
	zassert_true(capabilities & CAN_MODE_LISTENONLY, "LISTENONLY mode not supported");
	zassert_true(capabilities & CAN_MODE_ONE_SHOT, "ONE_SHOT mode not supported");
}

/**
 * @brief Test getting maximum filter count
 */
ZTEST(rockchip_can_init, test_get_max_filters)
{
	int max_filters;

	max_filters = can_get_max_filters(can_dev, false);
	zassert_true(max_filters > 0, "Invalid max_filters value");
	zassert_true(max_filters >= 1, "Must support at least 1 filter");
}

/**
 * @brief Test initial device state (before start)
 */
ZTEST(rockchip_can_init, test_initial_state)
{
	enum can_state state;
	struct can_bus_err_cnt err_cnt;
	int ret;

	ret = can_get_state(can_dev, &state, &err_cnt);
	zassert_equal(ret, 0, "Failed to get device state");
	zassert_equal(state, CAN_STATE_STOPPED, "Initial state should be STOPPED");
}

/**
 * @brief Test setting mode before start
 */
ZTEST(rockchip_can_init, test_set_mode_normal)
{
	int ret;

	ret = can_set_mode(can_dev, CAN_MODE_NORMAL);
	zassert_equal(ret, 0, "Failed to set NORMAL mode");
}

/**
 * @brief Test getting core clock frequency
 */
ZTEST(rockchip_can_init, test_get_core_clock)
{
	uint32_t clock_rate = 0;
	int ret;

	ret = can_get_core_clock(can_dev, &clock_rate);
	zassert_equal(ret, 0, "Failed to get core clock");
	zassert_not_equal(clock_rate, 0, "Core clock frequency should not be zero");
}

/**
 * @brief Test suite setup
 */
static void *rockchip_can_init_setup(void)
{
	test_setup();
	return NULL;
}

/**
 * @brief Test suite teardown
 */
static void rockchip_can_init_teardown(void *fixture)
{
	ARG_UNUSED(fixture);
	test_teardown();
}

ZTEST_SUITE(rockchip_can_init, NULL, rockchip_can_init_setup, NULL,
	    NULL, rockchip_can_init_teardown);
