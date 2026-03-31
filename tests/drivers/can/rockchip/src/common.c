/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Rockchip CAN Driver Tests - Common Implementation
 */

#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "common.h"

/* Global CAN device pointer */
ZTEST_DMEM const struct device *can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

/* Semaphores for callback synchronization */
ZTEST_DMEM struct k_sem rx_callback_sem;
ZTEST_DMEM struct k_sem tx_callback_sem;

/* Last received frame and filter info */
ZTEST_DMEM struct can_frame last_rx_frame = {0};
ZTEST_DMEM int last_rx_filter_id = -1;

/* Standard frame test data */
const struct can_frame test_std_frame_1 = {
	.flags = 0,
	.id = TEST_CAN_STD_ID_1,
	.dlc = 8,
	.data = {1, 2, 3, 4, 5, 6, 7, 8},
};

const struct can_frame test_std_frame_2 = {
	.flags = 0,
	.id = TEST_CAN_STD_ID_2,
	.dlc = 8,
	.data = {8, 7, 6, 5, 4, 3, 2, 1},
};

/* Extended frame test data */
const struct can_frame test_ext_frame_1 = {
	.flags = CAN_FRAME_IDE,
	.id = TEST_CAN_EXT_ID_1,
	.dlc = 8,
	.data = {10, 11, 12, 13, 14, 15, 16, 17},
};

const struct can_frame test_ext_frame_2 = {
	.flags = CAN_FRAME_IDE,
	.id = TEST_CAN_EXT_ID_2,
	.dlc = 8,
	.data = {17, 16, 15, 14, 13, 12, 11, 10},
};

/* RTR frame test data */
const struct can_frame test_std_rtr_frame_1 = {
	.flags = CAN_FRAME_RTR,
	.id = TEST_CAN_STD_ID_1,
	.dlc = 0,
	.data = {0},
};

const struct can_frame test_ext_rtr_frame_1 = {
	.flags = CAN_FRAME_IDE | CAN_FRAME_RTR,
	.id = TEST_CAN_EXT_ID_1,
	.dlc = 0,
	.data = {0},
};

/**
 * @brief Test setup - initialize semaphores and prepare device
 */
void test_setup(void)
{
	k_sem_init(&rx_callback_sem, 0, 10);
	k_sem_init(&tx_callback_sem, 0, 10);
	
	if (!device_is_ready(can_dev)) {
		ztest_test_skip();
	}
}

/**
 * @brief Test teardown - stop device and cleanup
 */
void test_teardown(void)
{
	int ret = can_stop(can_dev);
	zassert_true(ret == 0 || ret == -EALREADY, "Failed to stop CAN device");
}
