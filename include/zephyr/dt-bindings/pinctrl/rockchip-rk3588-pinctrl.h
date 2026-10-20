#ifndef _RK3588_PINCTRL_H_
#define _RK3588_PINCTRL_H_

#define RK_PINMUX(bank, pin, func) \
	((((bank) & 0xFFFF) << 16) | (((pin) & 0xFF) << 8) | ((func) & 0xFF))

/*
 * Drive strength levels for the RK3588 IOC drive-strength (DS) fields.
 *
 * These are continuous levels ordered from the weakest drive strength to the
 * strongest one, so device trees never have to encode the (non-monotonic)
 * register value themselves: the pinctrl driver maps the level onto the
 * hardware encoding.
 *
 *   level   impedance   DS field
 *   -----   ---------   --------
 *     1      100 Ohm     3'b000  (weakest)
 *     2       66 Ohm     3'b100
 *     3       50 Ohm     3'b010
 *     4       40 Ohm     3'b110  (hardware reset value)
 *     5       33 Ohm     3'b001
 *     6       25 Ohm     3'b101  (strongest)
 */
#define RK_DS_LEVEL_NONE 0 /* leave the reset value untouched */
#define RK_DS_LEVEL_1    1 /* 100 Ohm */
#define RK_DS_LEVEL_2    2 /* 66 Ohm */
#define RK_DS_LEVEL_3    3 /* 50 Ohm */
#define RK_DS_LEVEL_4    4 /* 40 Ohm */
#define RK_DS_LEVEL_5    5 /* 33 Ohm */
#define RK_DS_LEVEL_6    6 /* 25 Ohm */
#define RK_DS_LEVEL_MAX  RK_DS_LEVEL_6

#define RK_GPIO1 1
#define RK_GPIO3 3

#define RK_PB0 8
#define RK_PB1 9
#define RK_PB2 10
#define RK_PB3 11
#define RK_PB4 12
#define RK_PB5 13
#define RK_PB6 14
#define RK_PB7 15

#define RK_PD4 28

#define SPI0_CLK_M2    RK_PINMUX(RK_GPIO1, RK_PB3, 8)
#define SPI0_CS0_M2    RK_PINMUX(RK_GPIO1, RK_PB4, 8)
#define SPI0_MOSI_M2   RK_PINMUX(RK_GPIO1, RK_PB2, 8)
#define SPI0_MISO_M2   RK_PINMUX(RK_GPIO1, RK_PB1, 8)

#endif /* _RK3588_PINCTRL_H_ */