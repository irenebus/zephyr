/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Rockchip CAN Driver Tests - Common Definitions
 */

#ifndef ZEPHYR_TESTS_DRIVERS_CAN_ROCKCHIP_SRC_COMMON_H_
#define ZEPHYR_TESTS_DRIVERS_CAN_ROCKCHIP_SRC_COMMON_H_

#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

/* Test CAN IDs */
#define TEST_CAN_STD_ID_1 0x123
#define TEST_CAN_STD_ID_2 0x456
#define TEST_CAN_EXT_ID_1 0x12345678
#define TEST_CAN_EXT_ID_2 0x87654321

/* Test frames */
extern const struct can_frame test_std_frame_1;
extern const struct can_frame test_std_frame_2;
extern const struct can_frame test_ext_frame_1;
extern const struct can_frame test_ext_frame_2;
extern const struct can_frame test_std_rtr_frame_1;
extern const struct can_frame test_ext_rtr_frame_1;

/* Global variables */
extern const struct device *can_dev;
extern struct k_sem rx_callback_sem;
extern struct k_sem tx_callback_sem;
extern struct can_frame last_rx_frame;
extern int last_rx_filter_id;

/* Test utilities */
void test_setup(void);
void test_teardown(void);

#endif /* ZEPHYR_TESTS_DRIVERS_CAN_ROCKCHIP_SRC_COMMON_H_ */
