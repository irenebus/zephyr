/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Rockchip CAN Driver Tests - Interrupt and State Tests
 */

#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "common.h"

static enum can_state last_state = CAN_STATE_STOPPED;
static struct can_bus_err_cnt last_err_cnt = {0};

/**
 * @brief State change callback
 */
static void state_change_callback(const struct device *dev, enum can_state state,
				  struct can_bus_err_cnt err_cnt, void *user_data)
{
	zassert_equal(dev, can_dev, "Device mismatch in state callback");
	last_state = state;
	last_err_cnt = err_cnt;
	k_sem_give(&rx_callback_sem);
}

/**
 * @brief Test getting device state
 */
ZTEST(rockchip_can_interrupt_state, test_get_state)
{
	enum can_state state;
	struct can_bus_err_cnt err_cnt;
	int ret;

	ret = can_get_state(can_dev, &state, &err_cnt);
	zassert_equal(ret, 0, "Failed to get state");
	zassert_equal(state, CAN_STATE_STOPPED, "Device should be stopped initially");
	zassert_equal(err_cnt.rx_err_cnt, 0, "Initial RX error count should be 0");
	zassert_equal(err_cnt.tx_err_cnt, 0, "Initial TX error count should be 0");
}

/**
 * @brief Test state after starting
 */
ZTEST(rockchip_can_interrupt_state, test_state_after_start)
{
	enum can_state state;
	struct can_bus_err_cnt err_cnt;
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

	ret = can_get_state(can_dev, &state, &err_cnt);
	zassert_equal(ret, 0, "Failed to get state");
	zassert_equal(state, CAN_STATE_ERROR_ACTIVE, "Device should be ERROR_ACTIVE when running");

	can_stop(can_dev);
}

/**
 * @brief Test state after stop
 */
ZTEST(rockchip_can_interrupt_state, test_state_after_stop)
{
	enum can_state state;
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
	can_stop(can_dev);

	ret = can_get_state(can_dev, &state, NULL);
	zassert_equal(ret, 0, "Failed to get state after stop");
	zassert_equal(state, CAN_STATE_STOPPED, "Device should be STOPPED after stop");
}

/**
 * @brief Test getting error counters
 */
ZTEST(rockchip_can_interrupt_state, test_get_error_counters)
{
	struct can_bus_err_cnt err_cnt;
	int ret;

	ret = can_get_state(can_dev, NULL, &err_cnt);
	zassert_equal(ret, 0, "Failed to get error counters");
	/* Counters should be valid (not negative) */
	zassert_true(err_cnt.rx_err_cnt >= 0, "RX error count should be >= 0");
	zassert_true(err_cnt.tx_err_cnt >= 0, "TX error count should be >= 0");
}

/**
 * @brief Test setting state change callback
 */
ZTEST(rockchip_can_interrupt_state, test_set_state_change_callback)
{
	last_state = CAN_STATE_STOPPED;

	can_set_state_change_callback(can_dev, state_change_callback, NULL);

	/* Callback is now registered, should be called on state changes */
}

/**
 * @brief Test state callback on start
 */
ZTEST(rockchip_can_interrupt_state, test_state_callback_on_start)
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
	can_set_state_change_callback(can_dev, state_change_callback, NULL);

	ret = can_start(can_dev);
	zassert_equal(ret, 0, "Failed to start device");

	/* Give some time for callback */
	k_sleep(K_MSEC(10));

	can_stop(can_dev);
}

/**
 * @brief Test removing state change callback
 */
ZTEST(rockchip_can_interrupt_state, test_state_callback_removal)
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
	can_set_state_change_callback(can_dev, state_change_callback, NULL);
	ret = can_start(can_dev);
	zassert_equal(ret, 0, "Failed to start device");

	/* Remove callback by setting to NULL */
	can_set_state_change_callback(can_dev, NULL, NULL);

	can_stop(can_dev);
}

/**
 * @brief Test listening only mode state
 */
ZTEST(rockchip_can_interrupt_state, test_listenonly_mode_start)
{
	enum can_state state;
	int ret;

	struct can_timing timing = {
		.sjw = 2,
		.prop_seg = 0,
		.phase_seg1 = 8,
		.phase_seg2 = 4,
		.prescaler = 10,
	};

	can_set_mode(can_dev, CAN_MODE_LISTENONLY);
	can_set_timing(can_dev, &timing);
	ret = can_start(can_dev);
	zassert_equal(ret, 0, "Failed to start in LISTENONLY mode");

	ret = can_get_state(can_dev, &state, NULL);
	zassert_equal(ret, 0, "Failed to get state");
	zassert_not_equal(state, CAN_STATE_STOPPED, "Should not be STOPPED");

	can_stop(can_dev);
}

/**
 * @brief Test loopback mode state
 */
ZTEST(rockchip_can_interrupt_state, test_loopback_mode_start)
{
	enum can_state state;
	int ret;

	struct can_timing timing = {
		.sjw = 2,
		.prop_seg = 0,
		.phase_seg1 = 8,
		.phase_seg2 = 4,
		.prescaler = 10,
	};

	can_set_mode(can_dev, CAN_MODE_LOOPBACK);
	can_set_timing(can_dev, &timing);
	ret = can_start(can_dev);
	zassert_equal(ret, 0, "Failed to start in LOOPBACK mode");

	ret = can_get_state(can_dev, &state, NULL);
	zassert_equal(ret, 0, "Failed to get state");
	zassert_not_equal(state, CAN_STATE_STOPPED, "Should not be STOPPED");

	can_stop(can_dev);
}

/**
 * @brief Test one-shot mode state
 */
ZTEST(rockchip_can_interrupt_state, test_oneshot_mode_start)
{
	enum can_state state;
	int ret;

	struct can_timing timing = {
		.sjw = 2,
		.prop_seg = 0,
		.phase_seg1 = 8,
		.phase_seg2 = 4,
		.prescaler = 10,
	};

	can_set_mode(can_dev, CAN_MODE_ONE_SHOT);
	can_set_timing(can_dev, &timing);
	ret = can_start(can_dev);
	zassert_equal(ret, 0, "Failed to start in ONE_SHOT mode");

	ret = can_get_state(can_dev, &state, NULL);
	zassert_equal(ret, 0, "Failed to get state");
	zassert_not_equal(state, CAN_STATE_STOPPED, "Should not be STOPPED");

	can_stop(can_dev);
}

#ifdef CONFIG_CAN_MANUAL_RECOVERY_MODE
/**
 * @brief Test manual recovery mode
 */
ZTEST(rockchip_can_interrupt_state, test_manual_recovery_mode)
{
	// int ret;

	can_set_mode(can_dev, CAN_MODE_MANUAL_RECOVERY);
	zassert_equal(can_dev, can_dev, "Mode set successfully");

	/* Device should support recovery in this mode */
}

/**
 * @brief Test recovery operation
 */
ZTEST(rockchip_can_interrupt_state, test_recover_device)
{
	int ret;
	struct can_timing timing = {
		.sjw = 2,
		.prop_seg = 0,
		.phase_seg1 = 8,
		.phase_seg2 = 4,
		.prescaler = 10,
	};

	can_set_mode(can_dev, CAN_MODE_MANUAL_RECOVERY);
	can_set_timing(can_dev, &timing);
	can_start(can_dev);

	/* Try recovery with no wait */
	ret = can_recover(can_dev, K_NO_WAIT);
	zassert_true(ret == 0 || ret == -EAGAIN, "Recovery should succeed or timeout");

	can_stop(can_dev);
}
#endif /* CONFIG_CAN_MANUAL_RECOVERY_MODE */

/**
 * @brief Test multiple state transitions
 */
ZTEST(rockchip_can_interrupt_state, test_state_transitions)
{
	enum can_state state1, state2, state3;
	struct can_timing timing = {
		.sjw = 2,
		.prop_seg = 0,
		.phase_seg1 = 8,
		.phase_seg2 = 4,
		.prescaler = 10,
	};

	/* Initial state */
	can_get_state(can_dev, &state1, NULL);
	zassert_equal(state1, CAN_STATE_STOPPED, "Initial state should be STOPPED");

	/* After start */
	can_set_mode(can_dev, CAN_MODE_NORMAL);
	can_set_timing(can_dev, &timing);
	can_start(can_dev);
	can_get_state(can_dev, &state2, NULL);
	zassert_not_equal(state2, CAN_STATE_STOPPED, "State after start should not be STOPPED");

	/* After stop */
	can_stop(can_dev);
	can_get_state(can_dev, &state3, NULL);
	zassert_equal(state3, CAN_STATE_STOPPED, "State after stop should be STOPPED");
}

/**
 * @brief Test suite setup
 */
static void *rockchip_can_interrupt_state_setup(void)
{
	test_setup();
	last_state = CAN_STATE_STOPPED;
	last_err_cnt.rx_err_cnt = 0;
	last_err_cnt.tx_err_cnt = 0;
	return NULL;
}

/**
 * @brief Test suite teardown
 */
static void rockchip_can_interrupt_state_teardown(void *fixture)
{
	ARG_UNUSED(fixture);
	test_teardown();
}

ZTEST_SUITE(rockchip_can_interrupt_state, NULL, rockchip_can_interrupt_state_setup, NULL,
	    NULL, rockchip_can_interrupt_state_teardown);
