/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 KylinSoft Corporation
 * SPDX-License-Identifier: Apache-2.0
 *
 * RK3588 CRU reset identifiers used by the GMAC and CAN controllers.
 */

#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_RESET_RK3588_CRU_RESET_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_RESET_RK3588_CRU_RESET_H_

/* Zephyr reset IDs are "register index * 16 + bit" (11 * 16 + 8/9). */
#define SRST_P_CAN0 184
#define SRST_CAN0   185

#define SRST_A_GMAC0 522
#define SRST_A_GMAC1 523

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_RESET_RK3588_CRU_RESET_H_ */
