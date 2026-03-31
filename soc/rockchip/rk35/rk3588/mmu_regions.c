/*
 * Copyright (c) 2025 Syswonder
 * SPDX-License-Identifier: Apache-2.0
 * Copyright The Zephyr Project Contributors
 */

#include <zephyr/arch/arm64/arm_mmu.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

static const struct arm_mmu_region mmu_regions[] = {

	/*DRAM (128MB starting at 0x02000000) */
    MMU_REGION_FLAT_ENTRY("DRAM0",
                  DT_REG_ADDR(DT_NODELABEL(dram0)),
                  DT_REG_SIZE(DT_NODELABEL(dram0)),
                  MT_NORMAL | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE),

    /*GIC (Interrupt Controller) */
    MMU_REGION_FLAT_ENTRY("GIC_DIST", 
                  DT_REG_ADDR_BY_IDX(DT_NODELABEL(gic), 0),
                  DT_REG_SIZE_BY_IDX(DT_NODELABEL(gic), 0),
                  MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE),
    MMU_REGION_FLAT_ENTRY("GIC_REDIST", 
                  DT_REG_ADDR_BY_IDX(DT_NODELABEL(gic), 1),
                  DT_REG_SIZE_BY_IDX(DT_NODELABEL(gic), 1),
                  MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE),

    /*UART2 (Debug Console) */
    MMU_REGION_FLAT_ENTRY("UART2", 
                  DT_REG_ADDR(DT_NODELABEL(uart2)), 
                  DT_REG_SIZE(DT_NODELABEL(uart2)), 
                  MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE),

    /*CAN2 (Control Area Network) */
    MMU_REGION_FLAT_ENTRY("CAN2", 
                  DT_REG_ADDR(DT_NODELABEL(can2)), 
                  DT_REG_SIZE(DT_NODELABEL(can2)), 
                  MT_DEVICE_nGnRnE | MT_P_RW_U_NA | MT_DEFAULT_SECURE_STATE),

};

const struct arm_mmu_config mmu_config = {
	.num_regions = ARRAY_SIZE(mmu_regions),
	.mmu_regions = mmu_regions,
};
