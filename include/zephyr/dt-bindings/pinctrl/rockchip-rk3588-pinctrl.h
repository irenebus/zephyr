#ifndef _RK3588_PINCTRL_H_
#define _RK3588_PINCTRL_H_

#define RK_PINMUX(bank, pin, func) \
	((((bank) & 0xFFFF) << 16) | (((pin) & 0xFF) << 8) | ((func) & 0xFF))

#define RK_GPIO1 1

#define RK_PB0 8
#define RK_PB1 9
#define RK_PB2 10
#define RK_PB3 11
#define RK_PB4 12
#define RK_PB5 13
#define RK_PB6 14
#define RK_PB7 15

#define SPI0_CLK_M2    RK_PINMUX(RK_GPIO1, RK_PB3, 8)
#define SPI0_CS0_M2    RK_PINMUX(RK_GPIO1, RK_PB4, 8)
#define SPI0_MOSI_M2   RK_PINMUX(RK_GPIO1, RK_PB2, 8)
#define SPI0_MISO_M2   RK_PINMUX(RK_GPIO1, RK_PB1, 8)

#endif /* _RK3588_PINCTRL_H_ */