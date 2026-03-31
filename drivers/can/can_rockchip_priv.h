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

/* Register offsets (TRM chapter 29.4.2). */
#define RKCAN_MODE                  0x0000
#define RKCAN_CMD                   0x0004
#define RKCAN_STATE                 0x0008
#define RKCAN_INT                   0x000C
#define RKCAN_INT_MASK              0x0010
#define RKCAN_BITTIMING             0x0018
#define RKCAN_RXERRORCNT            0x0034
#define RKCAN_TXERRORCNT            0x0038
#define RKCAN_IDCODE                0x003C
#define RKCAN_IDMASK                0x0040
#define RKCAN_TXFRAMEINFO           0x0050
#define RKCAN_TXID                  0x0054
#define RKCAN_TXDATA0               0x0058
#define RKCAN_TXDATA1               0x005C
#define RKCAN_RXFRAMEINFO           0x0060
#define RKCAN_RXID                  0x0064
#define RKCAN_RXDATA0               0x0068
#define RKCAN_RXDATA1               0x006C

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

#define RKCAN_TX_FIFO_DEPTH         2U

#endif /* ZEPHYR_DRIVERS_CAN_CAN_ROCKCHIP_PRIV_H_ */
