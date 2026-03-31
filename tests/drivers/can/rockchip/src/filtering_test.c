/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Rockchip CAN Driver Tests - RX Filter Tests
 */

#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "common.h"

static int filter_rx_count = 0;

/**
 * @brief Generic RX callback for filter testing
 */
static void filter_rx_callback(const struct device *dev, struct can_frame *frame,
			       void *user_data)
{
	zassert_equal(dev, can_dev, "Device mismatch in RX callback");
	zassert_not_null(frame, "Frame is NULL in RX callback");
	
	filter_rx_count++;
	last_rx_frame = *frame;
	k_sem_give(&rx_callback_sem);
}

/**
 * @brief Test adding a basic RX filter
 */
ZTEST(rockchip_can_filtering, test_add_rx_filter_basic)
{
	int filter_id;
	struct can_filter filter = {
		.flags = 0,
		.id = TEST_CAN_STD_ID_1,
		.mask = CAN_STD_ID_MASK,
	};

	filter_id = can_add_rx_filter(can_dev, filter_rx_callback, NULL, &filter);
	zassert_true(filter_id >= 0, "Failed to add RX filter");

	can_remove_rx_filter(can_dev, filter_id);
}

/**
 * @brief Test adding multiple RX filters
 */
ZTEST(rockchip_can_filtering, test_add_multiple_filters)
{
	int filter_id1, filter_id2, filter_id3;
	struct can_filter filter = {0};

	filter.id = TEST_CAN_STD_ID_1;
	filter.mask = CAN_STD_ID_MASK;
	filter_id1 = can_add_rx_filter(can_dev, filter_rx_callback, NULL, &filter);
	zassert_true(filter_id1 >= 0, "Failed to add first filter");

	filter.id = TEST_CAN_STD_ID_2;
	filter_id2 = can_add_rx_filter(can_dev, filter_rx_callback, NULL, &filter);
	zassert_true(filter_id2 >= 0, "Failed to add second filter");

	filter.id = TEST_CAN_EXT_ID_1;
	filter.flags = CAN_FILTER_IDE;
	filter_id3 = can_add_rx_filter(can_dev, filter_rx_callback, NULL, &filter);
	zassert_true(filter_id3 >= 0, "Failed to add third filter");

	can_remove_rx_filter(can_dev, filter_id1);
	can_remove_rx_filter(can_dev, filter_id2);
	can_remove_rx_filter(can_dev, filter_id3);
}

/**
 * @brief Test adding filter with NULL callback fails
 */
ZTEST(rockchip_can_filtering, test_add_filter_null_callback)
{
	int filter_id;
	struct can_filter filter = {
		.flags = 0,
		.id = TEST_CAN_STD_ID_1,
		.mask = CAN_STD_ID_MASK,
	};

	filter_id = can_add_rx_filter(can_dev, NULL, NULL, &filter);
	zassert_equal(filter_id, -EINVAL, "Should reject NULL callback");
}

/**
 * @brief Test adding filter with NULL filter pointer fails
 */
ZTEST(rockchip_can_filtering, test_add_filter_null_filter)
{
	int filter_id;

	filter_id = can_add_rx_filter(can_dev, filter_rx_callback, NULL, NULL);
	zassert_equal(filter_id, -EINVAL, "Should reject NULL filter pointer");
}

/**
 * @brief Test removing a filter
 */
ZTEST(rockchip_can_filtering, test_remove_filter)
{
	int filter_id;
	struct can_filter filter = {
		.flags = 0,
		.id = TEST_CAN_STD_ID_1,
		.mask = CAN_STD_ID_MASK,
	};

	filter_id = can_add_rx_filter(can_dev, filter_rx_callback, NULL, &filter);
	zassert_true(filter_id >= 0, "Failed to add filter");

	/* Removing should not return any error  */
	can_remove_rx_filter(can_dev, filter_id);
}

/**
 * @brief Test removing with invalid filter ID
 */
ZTEST(rockchip_can_filtering, test_remove_filter_invalid_id)
{
	/* Should not crash or return error for invalid IDs */
	can_remove_rx_filter(can_dev, -1);
	can_remove_rx_filter(can_dev, 999);
}

/**
 * @brief Test standard frame filter
 */
ZTEST(rockchip_can_filtering, test_filter_standard_frame)
{
	int filter_id;
	struct can_filter filter = {
		.flags = 0,
		.id = TEST_CAN_STD_ID_1,
		.mask = CAN_STD_ID_MASK,
	};

	filter_id = can_add_rx_filter(can_dev, filter_rx_callback, NULL, &filter);
	zassert_true(filter_id >= 0, "Failed to add filter");

	can_remove_rx_filter(can_dev, filter_id);
}

/**
 * @brief Test extended frame filter
 */
ZTEST(rockchip_can_filtering, test_filter_extended_frame)
{
	int filter_id;
	struct can_filter filter = {
		.flags = CAN_FILTER_IDE,
		.id = TEST_CAN_EXT_ID_1,
		.mask = CAN_EXT_ID_MASK,
	};

	filter_id = can_add_rx_filter(can_dev, filter_rx_callback, NULL, &filter);
	zassert_true(filter_id >= 0, "Failed to add extended filter");

	can_remove_rx_filter(can_dev, filter_id);
}

/**
 * @brief Test filter with mask
 */
ZTEST(rockchip_can_filtering, test_filter_with_mask)
{
	int filter_id;
	struct can_filter filter = {
		.flags = 0,
		.id = TEST_CAN_STD_ID_1,
		.mask = 0x7E0,  /* Match 0x1C0 to 0x1FF */
	};

	filter_id = can_add_rx_filter(can_dev, filter_rx_callback, NULL, &filter);
	zassert_true(filter_id >= 0, "Failed to add masked filter");

	can_remove_rx_filter(can_dev, filter_id);
}

/**
 * @brief Test filling filter table
 */
ZTEST(rockchip_can_filtering, test_fill_filter_table)
{
	int max_filters;
	int filter_id;
	int filter_ids[16] = {0};
	struct can_filter filter = {
		.flags = 0,
		.id = TEST_CAN_STD_ID_1,
		.mask = CAN_STD_ID_MASK,
	};

	max_filters = can_get_max_filters(can_dev, false);
	zassert_true(max_filters > 0, "Max filters should be > 0");

	/* Try to add up to max_filters */
	for (int i = 0; i < max_filters; i++) {
		filter_id = can_add_rx_filter(can_dev, filter_rx_callback, NULL, &filter);
		if (i == 0) {
        	/* 针对第一个过滤器进行断言，如果失败则直接终止测试并打印错误码 */
        	zassert_true(filter_id >= 0, "Failed to add first filter (err: %d)", filter_id);
    	}	
		if (filter_id >= 0) {
			filter_ids[i] = filter_id;
		}
	}

	/* Clean up */
	for (int i = 0; i < max_filters; i++) {
		if (filter_ids[i] > 0) {
			can_remove_rx_filter(can_dev, filter_ids[i]);
		}
	}
}

/**
 * @brief Test adding filter beyond capacity returns ENOSPC
 */
ZTEST(rockchip_can_filtering, test_add_filter_exceeds_capacity)
{
	int max_filters;
	int filter_id;
	int filter_ids[16] = {0};
	struct can_filter filter = {
		.flags = 0,
		.id = TEST_CAN_STD_ID_1,
		.mask = CAN_STD_ID_MASK,
	};

	max_filters = can_get_max_filters(can_dev, false);

	/* Fill all filters */
	for (int i = 0; i < max_filters; i++) {
		filter_id = can_add_rx_filter(can_dev, filter_rx_callback, NULL, &filter);
		if (filter_id >= 0) {
			filter_ids[i] = filter_id;
		}
	}

	/* Try to add one more beyond capacity */
	filter_id = can_add_rx_filter(can_dev, filter_rx_callback, NULL, &filter);
	zassert_equal(filter_id, -ENOSPC, "Should return ENOSPC when filter table full");

	/* Clean up */
	for (int i = 0; i < max_filters; i++) {
		if (filter_ids[i] >= 0) {
			can_remove_rx_filter(can_dev, filter_ids[i]);
		}
	}
}

/**
 * @brief Test filter with user data
 */
ZTEST(rockchip_can_filtering, test_filter_with_user_data)
{
	int filter_id;
	void *test_user_data = (void *)0xDEADBEEF;
	struct can_filter filter = {
		.flags = 0,
		.id = TEST_CAN_STD_ID_1,
		.mask = CAN_STD_ID_MASK,
	};

	filter_id = can_add_rx_filter(can_dev, filter_rx_callback, test_user_data, &filter);
	zassert_true(filter_id >= 0, "Failed to add filter with user_data");

	can_remove_rx_filter(can_dev, filter_id);
}

/**
 * @brief Test suite setup
 */
static void *rockchip_can_filtering_setup(void)
{
	test_setup();
	filter_rx_count = 0;
	return NULL;
}

/**
 * @brief Test suite teardown
 */
static void rockchip_can_filtering_teardown(void *fixture)
{
	ARG_UNUSED(fixture);
	test_teardown();
}

ZTEST_SUITE(rockchip_can_filtering, NULL, rockchip_can_filtering_setup, NULL,
	    NULL, rockchip_can_filtering_teardown);
