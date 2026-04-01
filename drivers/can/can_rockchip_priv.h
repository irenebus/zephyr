/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_CAN_CAN_ROCKCHIP_PRIV_H_
#define ZEPHYR_DRIVERS_CAN_CAN_ROCKCHIP_PRIV_H_

#include <zephyr/sys/util.h>

/*
 * Placeholder bitrate range for the skeleton implementation.
 *
 * Replace these with the real min/max values from the Rockchip CAN IP
 * reference manual once timing register constraints are known.
 */
#define ROCKCHIP_CAN_MIN_BITRATE 10000U
#define ROCKCHIP_CAN_MAX_BITRATE 1000000U

/* Register map (TRM chapter 29.4.2). */
struct rkcan_tx_buf_regs {
	uint32_t frame_info;
	uint32_t id;
	uint32_t data0;
	uint32_t data1;
};

struct rkcan_filter_regs {
	uint32_t id_code;
	uint32_t id_mask;
};

struct rkcan_regs {
	uint32_t mode;                 /* 0x0000 */
	uint32_t cmd;                  /* 0x0004 */
	uint32_t state;                /* 0x0008 */
	uint32_t int_status;           /* 0x000c */
	uint32_t int_mask;             /* 0x0010 */
	uint32_t reserved_0014;        /* 0x0014 */
	uint32_t bittiming;            /* 0x0018 */
	uint32_t reserved_001c_0030[6]; /* 0x001c - 0x0030 */
	uint32_t rx_error_cnt;         /* 0x0034 */
	uint32_t tx_error_cnt;         /* 0x0038 */
	struct rkcan_filter_regs filter0; /* 0x003c - 0x0043 */
	uint32_t reserved_0044_004c[3]; /* 0x0044 - 0x004c */
	struct rkcan_tx_buf_regs tx_buf0; /* 0x0050 - 0x005c */
	uint32_t rx_frame_info;        /* 0x0060 */
	uint32_t rx_id;                /* 0x0064 */
	uint32_t rx_data0;             /* 0x0068 */
	uint32_t rx_data1;             /* 0x006c */
	struct rkcan_tx_buf_regs tx_buf1; /* 0x0070 - 0x007c */
	uint32_t reserved_0080_0118[39]; /* 0x0080 - 0x0118 */
	uint32_t afr_ctrl;             /* 0x011c */
	struct rkcan_filter_regs filters[5]; /* 0x0120 - 0x0147 */
};




/* CAN_MODE bits. */
#define RKCAN_MODE_AUTO_BUS_ON      BIT(11)
#define RKCAN_MODE_AUTO_RETX        BIT(10)
#define RKCAN_MODE_RXSTX            BIT(5)
#define RKCAN_MODE_LOOPBACK         BIT(4)
#define RKCAN_MODE_SILENT           BIT(3)
#define RKCAN_MODE_SELF_TEST        BIT(2)
#define RKCAN_MODE_SLEEP            BIT(1)
#define RKCAN_MODE_WORK             BIT(0)

/* CAN_CMD bits (R/WSC). */
#define RKCAN_CMD_TX1_REQ           BIT(1)
#define RKCAN_CMD_TX0_REQ           BIT(0)
#define RKCAN_CMD_TX_REQ(i)         (RKCAN_CMD_TX0_REQ << (i))

/* CAN_STATE bits. */
#define RKCAN_STATE_SLEEP           BIT(6)
#define RKCAN_STATE_BUS_OFF         BIT(5)
#define RKCAN_STATE_ERROR_WARNING   BIT(4)
#define RKCAN_STATE_TX_PERIOD       BIT(3)
#define RKCAN_STATE_RX_PERIOD       BIT(2)
#define RKCAN_STATE_TX_BUF_FULL     BIT(1)
#define RKCAN_STATE_RX_BUF_FULL     BIT(0)

/* CAN_INT bits (W1C). */
#define RKCAN_INT_BUS_OFF_RECOVERY  BIT(10)
#define RKCAN_INT_BUS_OFF           BIT(9)
#define RKCAN_INT_ERROR             BIT(6)
#define RKCAN_INT_ARB_FAIL          BIT(5)
#define RKCAN_INT_PASSIVE_ERROR     BIT(4)
#define RKCAN_INT_OVERLOAD          BIT(3)
#define RKCAN_INT_ERROR_WARNING     BIT(2)
#define RKCAN_INT_TX_FINISH         BIT(1)
#define RKCAN_INT_RX_FINISH         BIT(0)

/* CAN_BITTIMING fields. */
#define RKCAN_BITTIMING_SAMPLE_MODE BIT(16)
#define RKCAN_BITTIMING_SJW_MASK    GENMASK(15, 14)
#define RKCAN_BITTIMING_BRP_MASK    GENMASK(13, 8)
#define RKCAN_BITTIMING_TSEG2_MASK  GENMASK(6, 4)
#define RKCAN_BITTIMING_TSEG1_MASK  GENMASK(3, 0)

/* Frame info bits. */
#define RKCAN_FRAMEINFO_IDE         BIT(7)
#define RKCAN_FRAMEINFO_RTR         BIT(6)
#define RKCAN_FRAMEINFO_DLC_MASK    GENMASK(3, 0)

/* CAN_AFR_CTRL bits (controls filter pairs 1..5 at 0x011c). */
#define RKCAN_AFR_CTRL_UAF1         BIT(0)
#define RKCAN_AFR_CTRL_UAF2         BIT(1)
#define RKCAN_AFR_CTRL_UAF3         BIT(2)
#define RKCAN_AFR_CTRL_UAF4         BIT(3)
#define RKCAN_AFR_CTRL_UAF5         BIT(4)
#define RKCAN_AFR_CTRL_UAF_MASK     GENMASK(4, 0)
#define RKCAN_AFR_CTRL_UAF(filter_idx) BIT((filter_idx) - 1U)

#define RKCAN_TX_BUFFERS            2U
#define RKCAN_FILTER_BANKS          6U

#endif /* ZEPHYR_DRIVERS_CAN_CAN_ROCKCHIP_PRIV_H_ */
