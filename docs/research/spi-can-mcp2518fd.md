<!--
SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
SPDX-License-Identifier: Apache-2.0
-->

# SPI-CAN (MCP2518FD) 在 RK3588 上的依赖面

- 对应 issue：[irenebus/zephyr#4](https://github.com/irenebus/zephyr/issues/4)（`wayfinder:research`）
- 研究树：Zephyr `rk3588-bao` @ `e2810dc5658`（boards: baovm: add LubanCat4 VM1 board）；
  Linux `/home/yuhaonan/文档/repos/linux` @ `7c94261a67f`（Makefile 7.2.0）
- 方式：只读。未改驱动、未改 DTS、未建分支、未提交；本文所有结论标注了 primary source。
- 结论先行：**`spi_dw` 不能原样复用**（同一 IP 家族，但寄存器偏移和 CTRLR0 位域都不同）；
  最小可用路径是「CRU 门控 + pinctrl（可退化为板级写 IOC）+ Rockchip GPIO（带中断）+ 一个 PIO 轮询版 SPI 驱动」。

---

## 0. 现状盘点（树里已有什么）

| 部件 | 现状 | 证据 |
| --- | --- | --- |
| SPI0 节点 | 已有，`rockchip,rk3588-spi`，0xfeb00000，GIC SPI 326 | `dts/arm64/rockchip/rk3588s-baovm.dtsi:72-81` |
| SPI0 绑定/驱动 | **都没有**。`dts/bindings/spi/rockchip,rk3588-spi.yaml` 不存在，`drivers/spi/spi_rockchip.c` 不存在 | `ls dts/bindings/spi/`、`ls drivers/spi/` |
| Kconfig | `CONFIG_SPI_ROCKCHIP` / `CONFIG_PINCTRL_ROCKCHIP` / `CONFIG_GPIO_ROCKCHIP` **在树里都没有定义**，但板级 defconfig 已写 | `boards/baovm/lubancat4_vm1/lubancat4_vm1_defconfig:15-19`；`grep -rn "config SPI_ROCKCHIP" .` 无结果 |
| GPIO3 节点 | 已有，0xfec40000，GIC SPI 280，`rockchip,rk3588-gpio` | `dts/arm64/rockchip/rk3588s-baovm.dtsi:95-103` |
| GPIO 绑定/驱动 | **都没有** | `dts/bindings/gpio/` 无 rockchip；`drivers/gpio/` 无 rockchip |
| pinctrl 节点 + pinmux 头 | 已有，`rockchip,rk3588-pinctrl` + `spi0_m2` 组 | `dts/arm64/rockchip/rk3588s-baovm.dtsi:88-104`、`dts/arm64/rockchip/rk3588-pinctrl.dtsi:1-13`、`include/zephyr/dt-bindings/pinctrl/rockchip-rk3588-pinctrl.h:47-50` |
| pinctrl 绑定/驱动 | **都没有** | `dts/bindings/pinctrl/`、`drivers/pinctrl/` 无 rockchip |
| CRU 时钟/复位 | 有驱动但只覆盖 GMAC；**DTS 里没有 CRU 节点**；SPI0/GPIO3 的 clock id 未定义 | `drivers/clock_control/clock_control_rk3588_cru.c`、`drivers/reset/reset_rk3588_cru.c`、`include/zephyr/dt-bindings/clock/rk3588-cru.h` |
| MCP251xFD | 驱动和绑定都在（上游），板级节点已写好但 `status = "disabled"` | `drivers/can/can_mcp251xfd.c`、`dts/bindings/can/microchip,mcp251xfd.yaml`、`boards/baovm/lubancat4_vm1/lubancat4_vm1.dts:113-120` |
| CAN 配置 | defconfig 里没有任何 `CONFIG_CAN*` | `boards/baovm/lubancat4_vm1/lubancat4_vm1_defconfig`（全文 27 行） |

---

## 1. RK3588 SPI0 能否复用 `spi_dw`？

### 1.1 先确认硬件性质

- RK3588 SPI0 在 Linux 里的 compatible 是 `"rockchip,rk3588-spi", "rockchip,rk3066-spi"`，
  即走 rockchip 自家驱动，而不是 `snps,designware-spi`：
  `arch/arm64/boot/dts/rockchip/rk3588-base.dtsi:2744-2758`。
- 该驱动的寄存器块与 DW APB SSI 是**同源但改过布局**：CTRLR0/CTRLR1/SSIENR/IMR/ISR/RISR 偏移一致，
  SER 之后整体前移 4 字节，数据口拆成 TXDR/RXDR，VERSION 挪到 0x48。
  `drivers/spi/spi-rockchip.c:26-57`。
- 驱动里没有 rk3588 专属 compatible，只有 `rockchip,rk3066-spi` 等：
  `drivers/spi/spi-rockchip.c:1013-1029`。所以 RK3588 的差异全部由运行期 VERSION 判断处理
  （FIFO 深度 64/32：`drivers/spi/spi-rockchip.c:221-232`；CS_HIGH：`:888-905`）。

### 1.2 直接冲突的寄存器（决定性证据）

| 寄存器 | Linux rockchip | Zephyr `spi_dw` | 后果 |
| --- | --- | --- | --- |
| SER | 0x0c | 0x10（`spi_dw_regs.h:21`） | `spi_dw` 片选写进 **BAUDR** |
| BAUDR | 0x10 | 0x14（`:22`） | 波特率写进 **TXFTLR** |
| TXFTLR | 0x14 | 0x18（`:23`） | 阈值写进 **RXFTLR** |
| RXFTLR | 0x18 | 0x1c（`:24`） | 阈值写进 **TXFLR**（只读） |
| TXFLR | 0x1c | 0x20（`:25`） | 读到 **RXFLR** |
| RXFLR | 0x20 | 0x24（`:26`） | 读到 **SR** |
| SR | 0x24 | 0x28（`:27`） | `test_bit_sr_busy` 读的是 **IPR** |
| IPR | 0x28 | — | 无 |
| IMR/ISR/RISR | 0x2c/0x30/0x34 | 0x2c/0x30/0x34（`:28-30`） | 恰好一致 |
| ICR | 0x38 | 0x48（`:35`） | `clear_interrupts` 读的是 **VERSION**，中断清不掉 |
| DMACR/DMATDLR/DMARDLR | 0x3c/0x40/0x44 | 0x4c/0x50/0x54（`:36-38`） | 全错位 |
| VERSION | 0x48 | SSI_COMP_VERSION 0x5c（`:40`） | — |
| 数据口 | TXDR 0x400 / RXDR 0x800 | DR 0x60（`:41`） | 收发数据写进保留区，**完全不通** |

Linux 侧偏移见 `drivers/spi/spi-rockchip.c:26-57`；Zephyr 侧见 `drivers/spi/spi_dw_regs.h:17-42`。

### 1.3 CTRLR0 位域也不一样

- rockchip：DFS[3:0] 用枚举值（0=4bit、1=8bit、2=16bit），XFM 在 bit18（TR/TO/RO），
  另有 CSM(8)、SSD(10)、EM(11)、FBM(12)、BHT(13)、RSD(14)、OPM(20)、SOI(23)。
  `drivers/spi/spi-rockchip.c:60-116`（位定义）、`:529-613`（组装）。
- Zephyr：DFS = (bpw-1)，或者 32 位时放 [20:16]；TMOD 在 bit8（2 bit），SLV_OE(10)、SRL(11)。
  `drivers/spi/spi_dw.h:192-224`。
- 冲突点：Zephyr 的 TMOD(bit8-9) 正好压在 rockchip 的 CSM(bit8) 上；
  Zephyr 的 32 位 DFS[20:16] 压在 rockchip 的 XFM(18)/OPM(20) 上。

### 1.4 其余差异

| 维度 | rockchip | Zephyr spi_dw |
| --- | --- | --- |
| FIFO 深度 | 运行期读 VERSION 得 64/32（`spi-rockchip.c:221-232`） | DT 属性 `fifo-depth`，无默认值（`dts/bindings/spi/snps,designware-spi.yaml:24-32`） |
| 波特率 | 强制偶数分频 `2*DIV_ROUND_UP(f,2*s)`，范围 2..65534（`spi-rockchip.c:609-610`、`:134-136`） | `f/s` 向下取整，并拒绝 `f > clk/2`（`drivers/spi/spi_dw.h:152-153`、`spi_dw.c:236-240`） |
| 时钟 | `spiclk` + `apb_pclk` 两路，`max_sclk = min(freq/2, 50MHz)`（`spi-rockchip.c:790-800`、`:854-855`） | 单路 `clock_frequency`，只在 init 里 `clock_control_on`（`spi_dw.c:701` 附近的 DT 宏） |
| 复位 | **驱动完全不碰 reset**，DTS 也没有 `resets`（`spi-rockchip.c` 全文无 "reset"；`rk3588-base.dtsi:2744-2758`） | 无复位支持 |
| 中断 | 单条 GIC SPI 326，ISR 只使能 RX-full（`spi-rockchip.c:351-380`） | 单条中断可用（`spi_dw.c:663-676` 的 `DT_NUM_IRQS==1` 分支），但清中断方式不同 |
| 片选 | `SER` 位图，最多 2 个 native CS；也支持 GPIO CS（`spi-rockchip.c:236-275`、`:843-849`） | `write_ser(BIT(peripheral))`，无多 CS 语义（`spi_dw.c:463`） |
| 传输模式 | 按 tx/rx 组合设 XFM；PIO 由 RX-full 中断驱动，TX 侧预填 FIFO（`spi-rockchip.c:298-349`） | TMOD + TX-empty/RX-full 双阈值中断（`spi_dw.c:524-551`） |
| 字长 | 只支持 4/8/16（`spi-rockchip.c:853`） | 4..32 |

### 1.5 判定

**不能复用。** 不是"改几个 offset"，而是 SER/BAUDR/FIFO/SR/ICR/数据口全部错位，
且 CTRLR0 的传输模式位和字长编码不同。能复用的是**骨架**：`spi_context` 的缓冲/锁/完成模型、
"配置 → 填 FIFO → 开 SSIENR → 中断/轮询 → 关 SSIENR"的流程。

两条落地路线：

- **路线 A（推荐）：新写 `drivers/spi/spi_rockchip.c`，PIO 轮询，不用中断/DMA。**
  MCP251xFD 单次传输最长约 1.2 KB（CAN FD，见第 4 节），8 MHz 下约 1.2 ms，轮询完全可接受；
  这样连 ISR 都不用写，约 250–350 行。参照对象是 `drivers/spi/spi_bitbang.c`（纯阻塞 PIO）。
- **路线 B：把 `spi_dw.c` 参数化成"寄存器布局 + CTRLR0 编码"两个钩子。**
  改动会落在一个被 ARC/Intel/Nordic/Realtek 共用的驱动上，需要新增 binding 变体，
  回归面大；对本分支不一定值得。若做，至少要把 `spi_dw_regs.h`、`clear_interrupts`、
  `write_ser`、`write_baudr`、`fifo_depth` 五处抽象出来。

> 注意：`CONFIG_SPI_DW` 依赖 `DT_HAS_SNPS_DESIGNWARE_SPI_ENABLED`（`drivers/spi/Kconfig.dw:7-13`），
> 所以"给节点加 `snps,designware-spi` 别名"这种做法在 RK3588 上连编译都过不了语义关：
> `fifo-depth` / `max-xfer-size` 没有默认值，缺了就是 0。

---

## 2. GPIO：最小实现面

### 2.1 为什么绕不开

MCP251xFD 驱动在 init 里依次调用：

- `gpio_is_ready_dt(&dev_cfg->int_gpio_dt)` — `drivers/can/can_mcp251xfd.c:1579`
- `gpio_pin_configure_dt(..., GPIO_INPUT)` — `:1583`
- `gpio_add_callback_dt()` — `:1593`
- `gpio_pin_interrupt_configure_dt(..., GPIO_INT_LEVEL_ACTIVE)` — `:1599`
- 回调里 `GPIO_INT_DISABLE` 并 `k_sem_give`；线程处理完再重新使能 — `:1140-1180`
- 线程里还直接 `gpio_pin_get_dt()` 读 INT 电平 — `:1124`

也就是说 **GPIO 驱动必须同时具备：输入配置、port 级读、按 pin 的 level 中断、回调分发**。
单有 GPIO 输出/读、没有中断是不行的。Zephyr 树里也没有"通用 MMIO GPIO bank"驱动可借。

### 2.2 RK3588 GPIO 的硬件要点（决定实现量）

- RK3588 是 **GPIO v2** 布局，寄存器（相对 bank base）：
  ```
  port_dr 0x00, port_ddr 0x08, int_en 0x10, int_mask 0x18, int_type 0x20,
  int_polarity 0x28, int_bothedge 0x30, debounce 0x38, int_status 0x50,
  int_rawstatus 0x58, port_eoi 0x60, ext_port 0x70, version_id 0x78
  ```
  — `drivers/gpio/gpio-rockchip.c:49-69`；v2 判定靠读 `version_id`（`:670-692`，取值 `0x01000c2b/0x0101157c/0x010219c8`，`:35-37`）。
- v2 的每个逻辑寄存器是 **两个 32 位字**：低 16 位写值、高 16 位写使能（"写 1 才生效"）。
  Linux 的 `gpio_writel_v2()/gpio_readl_v2()` 就是干这个的：`drivers/gpio/gpio-rockchip.c:72-92`。
  **Zephyr 驱动不能简单 `sys_write32(read|val)`**，否则写不进去。
- 中断类型映射（level/edge + polarity）见 `rockchip_irq_set_type()`：`drivers/gpio/gpio-rockchip.c:385-470`；
  IT 位 0=level、1=edge；IP 位 0=low/falling、1=high/rising。
  MCP 用 `GPIO_ACTIVE_LOW` + level，落到硬件就是 level + polarity=0。
- 中断分发：整 bank 一条 GIC 中断（GPIO3 = SPI 280 → Zephyr IRQ 312，见
  `scripts/dts/gen_defines.py:498-508` 的 +32 规则），ISR 读 `int_status`(0x50) 后逐位派发，
  清中断写 `port_eoi`(0x60)：`drivers/gpio/gpio-rockchip.c:332-383` + `:548-550`。
- 时钟：`PCLK_GPIO3` = `CLKGATE_CON(17)` bit2（`drivers/clk/rockchip/clk-rk3588.c:1105-1106`），
  Zephyr 现有 CRU 驱动的寄存器映射是 `n*4 + 0x800`（`drivers/clock_control/clock_control_rk3588_cru.c:20`），
  即偏移 0x844 —— 加一条表项即可，但**必须先加**，否则 bank 可能完全不响应。

### 2.3 最小实现面

一个 `drivers/gpio/gpio_rockchip.c`：

- 必选 ops：`pin_configure`、`port_get_raw`、`port_set_masked_raw`、`port_set_bits_raw`、
  `port_clear_bits_raw`、`port_toggle_bits`（`include/zephyr/drivers/gpio.h` 的
  `struct gpio_driver_api`，5 个 mandatory，其余 optional）。
- 中断：`pin_interrupt_configure` + `manage_callback` + `get_pending_int`，ISR 里
  `gpio_fire_callbacks()`。
- 板级只需要 bank3 的 1 个 pin，但 API 面是全 bank 的，代码量约 **250–350 行** + binding 约 30 行。

参考体量：`drivers/gpio/gpio_sifive.c` 464 行、`drivers/gpio/gpio_bcm2711.c` 355 行。

---

## 3. pinctrl：最小实现

### 3.1 现有 DTS 已经写对了 pinmux 值

`spi0_m2` 组 = bank1 PB3(clk)/PB1(miso)/PB2(mosi)/PB4(cs0)，功能号 8：
`include/zephyr/dt-bindings/pinctrl/rockchip-rk3588-pinctrl.h:47-50`、`dts/arm64/rockchip/rk3588-pinctrl.dtsi:4-13`。
与 Linux 完全一致：`arch/arm64/boot/dts/rockchip/rk3588-base-pinctrl.dtsi:2771-2794`
（`spi0m2_pins`/`spi0m2_cs0`）。所以 **pinmux 数据不用重做，缺的只是"谁去写寄存器"**。

### 3.2 写 IOC 的语义（关键坑）

RK3588 的 IOC 寄存器也是"高 16 位写使能"：4 bit 宽的 mux 字段，
写入值必须是 `(mask << (bit+16)) | (func << bit)`，普通读改写不生效。
注释和实现：`drivers/pinctrl/pinctrl-rockchip.c:1283-1295`、`:1300-1420`。
4bit mux 的定位规则：`pin%8 >= 4` 时寄存器 +4，字段 bit = `(pin%4)*4`
（`:1204-1212`）；bank>0 时寄存器基址再 +0x8000（`:1238-1244`），
即落到 **BUS_IOC 0xfd5f8000**（Linux 的 `ioc` 是 0xfd5f0000，BUS_IOC = +0x8000：
`rk3588-base.dtsi:729-732`；Zephyr 里对应 `bus_ioc: syscon@fd5f8000`：
`dts/arm64/rockchip/rk3588s-baovm.dtsi:83-86`）。

由此推得 M2 四个脚的绝对地址（**推导值，需 TRM 复核**）：

| 信号 | 引脚 | 寄存器 | 字段 |
| --- | --- | --- | --- |
| SPI0_CLK_M2 | GPIO1_B3 (pin 11) | 0xfd5f8028 | bits[15:12] = 8 |
| SPI0_MOSI_M2 | GPIO1_B2 (pin 10) | 0xfd5f8028 | bits[11:8] = 8 |
| SPI0_MISO_M2 | GPIO1_B1 (pin 9) | 0xfd5f8028 | bits[7:4] = 8 |
| SPI0_CS0_M2 | GPIO1_B4 (pin 12) | 0xfd5f802c | bits[3:0] = 8 |

（bank1 的 iomux1 偏移 0x28、iomux2 偏移 0x30 来自 `pinctrl-rockchip.c:4281-4360` 的 offset 累加：
每个 iomux 8 字节、每 bank 4 个。）

### 3.3 两条路线

- **路线 A：完整 pinctrl 驱动。** `PINCTRL_SOC_PIN_T` 只需解 `bank/pin/func` 三个字段
  （`RK_PINMUX` 已经打好在 `rockchip-rk3588-pinctrl.h:4-5`），实现
  `pinctrl_configure_pins()` 写 IOC 即可；drive-strength / bias 可以先忽略或只实现 DS。
  体量参考 `drivers/pinctrl/pinctrl_bcm2711.c`（108 行）+ binding 约 40 行。
- **路线 B：板级直接写 IOC，不要 pinctrl 驱动。** 在 `soc/rockchip/rk35/rk3588/soc.c`
  （或板级 `board.c`）的 `SYS_INIT` 里写死上表 4 个字段，然后把 `&spi0` 的
  `pinctrl-0/pinctrl-names` 去掉、`CONFIG_PINCTRL` 关掉。约 30–40 行。
  代价：不走 Zephyr 的 pinctrl 模型，后续加别的外设要重复写；好处是省一个 binding + 一个驱动。

**INT 脚（GPIO3_D4，pin 28）** 目前没有任何 pinctrl 项。它作为 GPIO 输入需要 mux=0；
如果复位默认就是 0 可以不动，但更稳妥是补一个 `gpio3_d4` 组（func 0 + bias-pull-up），
否则 INT 线浮空会误触发。这一条是板级硬件问题，需对照原理图。

---

## 4. MCP251xFD 的要求与现有板级配置审计

### 4.1 驱动对 SPI 主机的要求

| 要求 | 值/说明 | 证据 |
| --- | --- | --- |
| 模式 | SPI mode 0（DTS 未写 `spi-cpol/spi-cpha`），8 bit 字长 | `drivers/can/can_mcp251xfd.c:1791`（`SPI_DT_SPEC_INST_GET(inst, SPI_WORD_SET(8))`） |
| 频率 | 受 errata 限制：`FSCK <= 0.85 * (FSYSCLK/2)`；20 MHz 晶振 → **8.5 MHz 上限** | Linux 同款注释 + 已知good组合表：`drivers/net/can/spi/mcp251xfd/mcp251xfd-core.c:2316-2363`；Zephyr 的 clamp 只在 PLL 模式下做：`can_mcp251xfd.c:1563-1571` |
| 传输形态 | 每条命令一次 `spi_transceive`（tx/rx 等长），写寄存器用 tx-only `spi_write` | `:75`、`:119`、`:156` |
| 单次长度 | RX FIFO 批量读：classic 约 `16*(8+8)+3 ≈ 320` B；CAN FD 约 `16*(8+64)+3 ≈ 1.2 KB`（`CAN_MAX_DLEN` 随 `CONFIG_CAN_FD_MODE` 为 8/64） | `drivers/can/can_mcp251xfd.h:20-60`、`:463-476`；`include/zephyr/drivers/can.h:75-77` |
| 片选 | 整条命令期间 CS 必须保持有效（不能每字节翻转） | 命令带 2 字节指令头 + 长度 + 数据，见 `can_mcp251xfd.h:430-435` |
| CRC | 读路径用 `READ_CRC` 指令，软件 CRC-16/0x8005、seed 0xFFFF，失败重试 5 次 | `can_mcp251xfd.h:55-56`、`can_mcp251xfd.c:85-140`、`Kconfig.mcp251xfd:48-52` |
| FIFO | 芯片 RAM 2 KB：CAN FD 下 TEF 8×8 + TX 8×(8+64) + RX 16×(12+64) = 1856 B（classic 为 512 B），编译期断言放得下 | `can_mcp251xfd.h:20-58` |
| CRC 使能 | CAN FD 的 ISO CRC 由驱动打开（`CON_ISOCRCEN`），与 SPI 主机无关 | `can_mcp251xfd.c:1340` |
| 晶振 | `osc-freq` 必填；`pll-enable` 缺省关，SYSCLK = osc | `dts/bindings/can/microchip,mcp251xfd.yaml:12-32`、`can_mcp251xfd.c:725-730` |

结论：**SPI 主机这边没有"特殊 FIFO/CRC 要求"**，只要能 mode-0、8 bit、整帧保持 CS、
把整条命令（≤ 1.2 KB）完整收发即可。真正卡人的是 errata 频率上限和 INT 中断。

### 4.2 现有板级 DTS 够不够

`boards/baovm/lubancat4_vm1/lubancat4_vm1.dts:108-120`：

- ✅ `osc-freq = <20000000>` 与驱动一致；没有开 `pll-enable`，20 MHz 直接做 SYSCLK 是对的
  （开了的话驱动会算 `osc*10 = 200 MHz`，见 `:725-730`，远超芯片规格）。
- ✅ `int-gpios = <&gpio3 RK_PD4 GPIO_ACTIVE_LOW>` 与驱动 `GPIO_INT_LEVEL_ACTIVE` 的语义吻合。
- ✅ `zephyr,canbus = &mcp2518fd0` 已指向该节点（`:22`），FIFO 默认参数放得下。
- ❌ **`spi-max-frequency = <10000000>` 超过 errata 上限 8.5 MHz。**
  驱动只在 `pll_enable` 时 clamp（`can_mcp251xfd.c:1563-1571`），这里 PLL 关着，所以 10 MHz 会原样下发；
  Linux 是不管 PLL 开不开都 clamp 到 `freq/2*0.85`（`mcp251xfd-core.c:2340-2346`）。
  建议改成 `8500000`（200 MHz 父时钟 + 偶数分频 → 8.33 MHz，正好是 Linux 表里的 83.33% 组合）。
- ❌ 节点 `status = "disabled"`，且 defconfig 没有 `CONFIG_CAN=y` / `CONFIG_CAN_MCP251XFD=y`
  （`CONFIG_CAN_MCP251XFD` 默认 y 但依赖 `DT_HAS_MICROCHIP_MCP251XFD_ENABLED`：
  `drivers/can/Kconfig.mcp251xfd:7-14`）。
- ⚠️ 没有 `clocks` 属性 → 驱动按外部晶振处理（正确），但也意味着没有任何时钟门控由软件负责。
- ⚠️ `num-cs = <2>` 但只配了 CS0 的 pinmux，用 CS0 时没问题；不要改成 `cs-gpios`，
  否则需要 SPI 驱动支持 GPIO CS（`spi_dw` 有，新驱动也要有）。

---

## 5. 与原生 CAN 路线对比

| 维度 | SPI-CAN（MCP2518FD） | 原生 CAN（RK3588 CAN-FD） |
| --- | --- | --- |
| Zephyr 控制器驱动 | **已有**（`can_mcp251xfd.c`，上游维护） | **没有**；`drivers/can/` 无 rockchip |
| Zephyr 平台依赖 | CRU 门控 + pinctrl + GPIO + SPI（4 项） | CRU 门控 + pinctrl + CAN（3 项） |
| Linux 参考驱动 | `spi-rockchip.c` 1044 行，PIO 路径完整 | 本树 `drivers/net/can/rockchip/` 2159 行（.c 1606 + .h 553），但本树 of_match 只有 `rk3568v2/v3`（`rockchip_canfd-core.c:844-848`） |
| Linux DTS 参考 | RK3588 SPI0 节点完整（`rk3588-base.dtsi:2744-2758`），但**没有任何 MCP2518FD 节点**（全树 grep 只在 freescale/qcom/mediatek 板上有） | **本树里 RK3588 根本没有 CAN 节点**（`grep "can@fea" rk3588-*.dtsi` 无结果）；上游 master 已加 `rk3588-canfd` devtype 与 compatible，但 DTS 节点仍未合入：<https://raw.githubusercontent.com/torvalds/linux/master/drivers/net/can/rockchip/rockchip_canfd-core.c>（devtype 在第 53 行、compatible 在第 864 行） |
| 硬件确定性 | 引脚/CS/INT 只由我们自己的 DTS 声明，Linux/vendor DTS 无法交叉验证 | 需要确认板上有 CAN 收发器；本树无任何证据 |
| 主要风险 | 4 个平台驱动都要新写，但每个都很小；SPI 侧行为可用逻辑分析仪逐步验证 | 一个 800–1000 行的 CAN 控制器驱动 + 从 TRM 补 DTS（地址/中断/时钟），且 IP 不是 Bosch M_CAN，**无法复用 `can_mcan`** |
| 到"能收一帧"的估算 | 2–3 周（估算） | 同仓另一份笔记估 classic 8–13 人日（含 pinctrl），见下 |

补充：

- 原生 CAN 的详细移植面（含 FD 状态、TDC erratum、寄存器/时钟清单）见同目录
  `docs/research/rk3588-native-can.md`（issue #3）。该笔记记录了上游 master 已有
  `rockchip,rk3588-canfd`（TDC erratum 修复后 FD 可用），只是 DTS 节点未合。
- 上游 Linux 在 2026 年有 "add CAN-FD nodes for RK3588" 的补丁系列
  （例：<https://lists.infradead.org/pipermail/linux-arm-kernel/2026-July/1144838.html>），
  但**本树没有合入**，所以原生 CAN 路线在本地没有可抄的 DTS。

---

## 6. 建议的最小依赖切片（按依赖顺序）

1. **CRU**：给 `rk3588-cru.h` 加 `CLK_SPI0(151)`、`PCLK_SPI0(146)`、`PCLK_GPIO3`，
   在 `clock_control_rk3588_cru.c` 的 lookup 表里加 `CON(14) bit6/bit11`、`CON(17) bit2`
   （编号与位来自 `clk-rk3588.c:1174-1186`、`:1105-1106`），并在 DTS 里补 CRU 节点
   `0xfd7c0000`（`rk3588-base.dtsi:742-744`）。约 40 行。
2. **pinctrl**：路线 A（驱动，约 150 行）或路线 B（板级 IOC 写死，约 40 行）。
3. **GPIO**：必须做，约 250–350 行 + binding。
4. **SPI**：新驱动，PIO 轮询版约 250–350 行；若坚持中断版再加 ~100 行。
5. **DTS/defconfig**：补 `fifo-depth`/`max-xfer-size`（若走 spi_dw 风格）、时钟、`status="okay"`、
   `CONFIG_CAN=y`、`CONFIG_CAN_MCP251XFD=y`、把 `spi-max-frequency` 降到 8.5 MHz。

合计新增约 **700–1000 行**（含 binding/Kconfig），比原生 CAN 路线小一个量级。

---

## 7. 不确定项（必须在动手前确认）

1. **Bao 的 MMIO 透传范围**（最高优先级）。所有结论都假设该 VM 能访问
   0xfeb00000（SPI0）、0xfec40000（GPIO3）、0xfd5f8000（BUS_IOC）、0xfd7c0000（CRU）。
   这几个仓库里没有本板的 Bao 配置（`bao-project` 只有 demo），无法验证。
   如果 CRU/IOC 没映射进来，上面第 1、2 步直接不可行，只能靠固件预置。
2. **板级硬件连接**：INT=GPIO3_D4、CS=CS0、20 MHz 晶振这三条只有我们自己的 Zephyr DTS 声明，
   Linux 树里没有任何 MCP2518FD 节点可对照（`grep -rn mcp2518 arch/arm64/boot/dts/rockchip/` 为空）。
   需要原理图确认，尤其是 INT 是否有外部上拉。
3. **寄存器语义只有 Linux 驱动这一手来源**，没有 RK3588 TRM。第 3.2 节推出的
   0xfd5f8028/0xfd5f802c 是推导值，需实测或 TRM 复核。
4. **SPI0 的 CRU 门控在 Zephyr 启动时是否已经打开**未知；若 Bao 或固件已开则第 1 步可省。
5. **SPI mode / SCK 上限的 datasheet 原文**未能离线核对（alldatasheet 被 Cloudflare 拦，
   Microchip PDF 未解析）。本文的 8.5 MHz 上限来自 Linux 驱动里的 errata 注释与实测组合表，
   属高可信但不是 datasheet 原文。
6. **未编译、未上板**：本环境没有 west、没有 Zephyr SDK（`which west` 为空），
   所以"defconfig 引用了不存在的 symbol 会怎样"只能从 Kconfig 文本判断，未实测；
   也未验证 DT 缺 binding 时构建是 warning 还是 error。
7. **MCP2518FD 是否真的贴在该板 / 是哪个封装**未知。

---

## 8. 主要来源

Zephyr（`/home/yuhaonan/文档/repos/zephyrproject/zephyr`）：

- `dts/arm64/rockchip/rk3588s-baovm.dtsi`（SPI0/GPIO3/pinctrl/bus_ioc 节点）
- `boards/baovm/lubancat4_vm1/lubancat4_vm1.dts`、`lubancat4_vm1_defconfig`
- `drivers/spi/spi_dw.c`、`spi_dw.h`、`spi_dw_regs.h`、`drivers/spi/Kconfig.dw`
- `dts/bindings/spi/snps,designware-spi.yaml`
- `drivers/can/can_mcp251xfd.c`、`can_mcp251xfd.h`、`drivers/can/Kconfig.mcp251xfd`
- `dts/bindings/can/microchip,mcp251xfd.yaml`
- `include/zephyr/dt-bindings/pinctrl/rockchip-rk3588-pinctrl.h`
- `drivers/clock_control/clock_control_rk3588_cru.c`、`drivers/reset/reset_rk3588_cru.c`
- `scripts/dts/gen_defines.py`（GIC SPI +32 规则）

Linux（`/home/yuhaonan/文档/repos/linux`）：

- `drivers/spi/spi-rockchip.c`
- `drivers/pinctrl/pinctrl-rockchip.c`
- `drivers/gpio/gpio-rockchip.c`
- `drivers/clk/rockchip/clk-rk3588.c`
- `drivers/net/can/spi/mcp251xfd/mcp251xfd-core.c`
- `drivers/net/can/rockchip/`
- `arch/arm64/boot/dts/rockchip/rk3588-base.dtsi`、`rk3588-base-pinctrl.dtsi`

外部：

- <https://github.com/irenebus/zephyr/issues/4>
- <https://lists.infradead.org/pipermail/linux-arm-kernel/2026-July/1144838.html>（RK3588 CAN-FD DTS 补丁）

