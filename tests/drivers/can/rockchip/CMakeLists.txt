# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rockchip_can_testing)

target_sources(app PRIVATE src/common.c)
target_sources(app PRIVATE src/init_test.c)
target_sources(app PRIVATE src/mode_timing_test.c)
target_sources(app PRIVATE src/tx_rx_test.c)
target_sources(app PRIVATE src/filtering_test.c)
target_sources(app PRIVATE src/interrupt_state_test.c)

# Include paths
target_include_directories(app PRIVATE src)
