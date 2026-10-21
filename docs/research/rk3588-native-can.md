<!--
SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
SPDX-License-Identifier: Apache-2.0
-->

# RK3588 原生 CAN（classic）Zephyr 移植面

Issue: irenebus/zephyr#3 — 「量化 RK3588 原生 CAN FD 的 Zephyr 移植面」
状态: 研究笔记（不含代码改动）。日期: 2026-10-20。分支: `rk3588-bao`（Zephyr 4.5.0-rc2）。
位置说明: 按票面要求放在 `docs/research/`；该目录为本票新建（仓库此前只有 `docs/agents/`），没有既有研究笔记目录可循。

路径约定（本文所有行号引用）：

- `linux:` = `../../linux`（Collabora `rockchip-3588` 树，分支 `baovm_lubancat4`，VERSION 7.2.0，见 `linux:Makefile:2-4`）
- `zephyr:` = 本仓库根（/home/yuhaonan/文档/repos/zephyrproject/zephyr）
- `bao:` = `../../bao-project`

---

## 0. 结论摘要（TL;DR）

1. **最小切片按 classic CAN 做是对的，但「RK3588 不能 CAN FD」这个前提已被上游推翻。** 2026-06-30 的 v1 系列（Heiko Stuebner）确实给 RK3588 打了 `RKCANFD_QUIRK_CANFD_BROKEN`；2026-07-30 的 v6 系列（Cunhao Lu）发现 BRS bus-off 是 **TDC（发射延迟补偿）配置** 造成的，改成 RK3588 上把 `RKCANFD_REG_TRANSMIT_DELAY_COMPENSATION` 写 0 后 CAN FD 可用，并实测 5 Mbit/s 数据段跑 15 分钟零错误。该结论已进入 `torvalds/linux` master：`rkcanfd_devtype_data_rk3588.quirks = ERRATUM_5 | ERRATUM_6`（无 `CANFD_BROKEN`），`of_match` 有 `rockchip,rk3588-canfd`。详见 §4。
2. **但 RK3588 的 DT 节点还没进 master。** 抓取 `torvalds/linux` master 的 `arch/arm64/boot/dts/rockchip/rk3588-base.dtsi`（99738 字节）与 `rk3588-extra.dtsi`，**没有** 任何 `can@fea5/6/7…` 节点；`rk3588-base-pinctrl.dtsi` 里 can0/1/2 的 m0/m1 引脚定义倒是在（第 70-127 行）。即：驱动 + binding 已合并，DTS 补丁尚未合并。
3. **本仓库的 Zephyr 侧目前没有可复用的原生 CAN 基础设施**：RK3588 CRU 只登记了 9 个 GMAC 时钟 ID、2 个 GMAC 复位 ID（`zephyr:include/zephyr/dt-bindings/clock/rk3588-cru.h:11-19`、`zephyr:include/zephyr/dt-bindings/reset/rk3588-cru-reset.h:11-12`），CRU 驱动是硬编码查表且 **没有 set_rate**（`zephyr:drivers/clock_control/clock_control_rk3588_cru.c:30-82,121-150`），**pinctrl 驱动根本不存在**（全树搜不到 `PINCTRL_ROCKCHIP` 的定义，只有板级 defconfig 引用），`dts/bindings/can/` 下没有 Rockchip 绑定，`drivers/can/` 下没有 Rockchip 驱动。
4. **`can_mcan` 不适用。** 它是 Bosch M_CAN IP 的后端（`zephyr:drivers/can/can_mcan.h:20-24`，`CREL/ENDN/CUST` 寄存器），Rockchip CANFD 是另一套寄存器（`linux:drivers/net/can/rockchip/rockchip_canfd.h:23-288`）。需要新写 `drivers/can/can_rockchip_canfd.c`。
5. **最大隐藏工作量不是 CAN 驱动，而是 pinmux**：Zephyr 无 RK3588 pinctrl 驱动（IOC / `bus_ioc` GRF，4 bit/pin iomux），CAN m0/m1 的引脚复用必须靠它；现状只能退化成「在 CAN 驱动里直接写 IOC 寄存器」这种不可上游的做法。
6. **建议：GO（classic-only 最小切片），NO-GO（首个切片含 FD）。** classic 切片估 8-13 人日（含 pinctrl），若 pinmux 另有人做则 5-8 人日；FD 追加 1-3 人日但 **必须先有硬件复核 TDC=0**。风险与未知见 §8、§9。

---

## 1. 票面范围与本文件的校正

票面要求「以 Linux `drivers/net/can/rockchip/rockchip_canfd*`（含 2026 年 RK3588 variant 补丁）为蓝本，给出最小移植面」，并明确最小切片 = classic、FD 不作为首切片交付。本文遵守该切片，但 §4 会给出 FD 状态的时间线，因为「FD 不可用」会直接影响 #7（Bao/VM 侧）对带宽与用例的判断。

需要特别指出：**任务交办时引用的 v1 系列（2026-06-30）不是最终状态**。v1 的 `RKCANFD_QUIRK_CANFD_BROKEN` 在 v4/v6 被移除。如果票面/下游文档继续写「RK3588 不支持 FD」，需要附上版本说明（见 §4.3）。

---

## 2. Linux 参考驱动解剖（本仓库 Linux 树，2159 行）

文件与规模（`linux:drivers/net/can/rockchip/`）：

| 文件 | 行数 | 职责 |
|---|---|---|
| `rockchip_canfd.h` | 553 | 全部寄存器定义、erratum 说明、私有结构 |
| `rockchip_canfd-core.c` | 962 | probe、时钟/复位、位时序、模式、中断、错误/状态机、ethtool 注册 |
| `rockchip_canfd-rx.c` | 299 | RX FIFO 读取、RXSTX 自收过滤（erratum 6）、空 FIFO 检测（erratum 5） |
| `rockchip_canfd-tx.c` | 167 | 2 深 TX mailbox、echo skb、TX 完成 |
| `rockchip_canfd-timestamp.c` | 105 | 32 bit 自由运行计数器 → cyclecounter/timecounter |

### 2.1 寄存器块（`linux:drivers/net/can/rockchip/rockchip_canfd.h`）

一个 4 KiB 窗口（DT `reg` 长度 0x1000）。关键块：

- `0x000 MODE`：`CAN_FD_MODE_ENABLE`(15)、`AUTO_RETX`(10)、`COVER_MODE`(8)、`RXSTX`(5)、`LBACK`(4)、`SILENT`(3)、`SELF_TEST`(2)、`WORK_MODE`(0)（h:23-39）。
- `0x004 CMD`：只有 `TX0_REQ`/`TX1_REQ` 两位 → **TX 是 2 深 mailbox**，`RKCANFD_TXFIFO_DEPTH 2`（h:41-44、h:292）。
- `0x008 STATE`、`0x00c INT`（15 个中断源，写 1 清零；h:55-93）、`0x010 INT_MASK`（1 = 屏蔽）。
- `0x018 BITTIMING`（classic 位时序；h:101-106）。
- `0x02c ERROR_CODE`（29 位错误现场，h:108-153）、`0x034/0x038` 错误计数器、`0x03c/0x040` 全局 ID/mask。
- `0x050..0x06c` classic TX/RX 帧窗口；`0x200..0x244` FD TX（16 个数据字）；`0x300..0x348` FD RX。
- `0x100/0x104` FD nominal / data 位时序；`0x108` TDC；`0x10c/0x110` 时间戳；`0x114` TX event FIFO（Linux 未用）；`0x118 RX_FIFO_CTRL`；`0x11c` AFC；`0x120..0x144` 5 组扩展 ID/mask。
- `0x400 RX_FIFO_RDATA`、`0x500 TXE_FIFO_RDATA`（h:287-288）：RX 每个元素 = 3 个 header 字（frameinfo/id/ts）+ 64 字节数据，由 `readsl` 连续读出（`linux:...-rx.c:210-215`）。

### 2.2 模式与启动序列（`linux:...-core.c:249-312`）

`chip_start()` 的顺序是移植时最该照抄的部分：

1. `reset_control_assert` + 2 µs + `deassert`，然后 `MODE=0`（`core.c:103-110`）。
2. 接收过滤全通：`IDCODE=0`、`IDMASK=0x1fffffff`（`core.c:256-257`）。
3. `MODE = CAN_FD_EN | AUTO_RETX | COVER_MODE | RXSTX | WORK_MODE`；loopback 再加 `LBACK|SILENT|SELF_TEST`（`core.c:266-277`）。
4. 默认屏蔽 `TIMESTAMP_OVF | TX_ARBIT_FAIL | OVERLOAD | TX_FINISH`；未开 `BERR_REPORTING` 时再屏蔽 `ERROR_INT`（`core.c:285-295`）。
5. `RX_FIFO_CTRL |= ENABLE`；`tx_head/tx_tail=0`（`core.c:235-247`）。
6. 时间戳启动 + 位时序 + 先关中断再进工作模式（`core.c:300-306`）。

停止：`MODE=0`（软复位）+ 屏蔽全部中断（`core.c:314-320`）。

### 2.3 位时序与 TDC（`core.c:114-160`）

- 时钟公式：`Tsclk = 2 × Tclk × (brp + 1)`，所以驱动里的 `brp_min/max/inc` 都是 datasheet 值的 2 倍（`core.c:78-115` 注释与常量）。
- nominal：`SJW=sjw-1`、`BRP=brp/2-1`、`TSEG2=phase_seg2-1`、`TSEG1=prop_seg+phase_seg1-1`（`core.c:126-135`）。
- data（FD）：同上，字段位置不同（h:193-198）。
- TDC：`tdco = (clock/data_bitrate)*2/3`，clamp 到 6 bit，且 `TDC_ENABLE=1`（`core.c:151-157`）。**RK3588 例外：写 0**（见 §4.3）。

### 2.4 中断与错误处理（`core.c:650-707`）

- 读 `INT`，与 `INT_MASK` 取反后得活动位；**先写 1 清除再处理**（避免快速重入丢中断，`core.c:661-664`）。
- 分发：`RX_FINISH` → RX 循环；`ERROR_INT` → 错误帧；`BUS_OFF/PASSIVE/ERROR_WARNING` 或状态已非 ERROR_ACTIVE → 状态机；`RX_FIFO_OVERFLOW` → 溢出错误帧。
- 错误计数器有「读回 0 时用软件缓存值」的 workaround（`core.c:162-203`），并在每次成功 RX/TX 后手动减计数（`rx.c:250-256`、`tx.c:153-158`）。

### 2.5 TX 路径与 erratum 6（`tx.c`、`rx.c:94-189`）

- `start_xmit`：写 `FD_TXFRAMEINFO / FD_TXID / FD_TXDATA*`，`can_put_echo_skb` 存 echo，`tx_head++`，再写 `CMD=TX_REQ(tx_head & 1)`（`tx.c:117-133`）。
- **TX 完成不靠 `TX_FINISH_INT`（它被屏蔽），而是靠 RXSTX 自收**：收到的帧与 echo skb 头/数据逐字节相同 → 认为发送完成（`rx.c:116-133`）。erratum 6（扩展帧被发成标准帧）也在这里检测并重发（`rx.c:135-188`）。
- erratum 5（FIFO 计数偏大）：读到元素 3 个字全相同即判空并退出（`rx.c:191-226`）。

### 2.6 时间戳（`timestamp.c:40-105`）

`TIMESTAMP_CTRL` 预分频到 ≥ 2× bitrate，32 bit 计数器，`cyclecounter` + `timecounter`；因 32 bit 在 300 MHz 下约 14 s 回绕，用 delayed work 定期读。Zephyr 首切片可只保留硬件时间戳读取 + 系统时间换算，或先不做时间戳。

---

## 3. RK3588 平台事实（地址 / 中断 / 时钟 / 复位 / 引脚）

### 3.1 DT 节点（来源：2026-06-30 v1 系列 patch 5，已由 v6 与合并版沿用）

    can0: can@fea50000 { reg = <0x0 0xfea50000 0x0 0x1000>;
        interrupts = <GIC_SPI 341 IRQ_TYPE_LEVEL_HIGH 0>;
        clocks = <&cru CLK_CAN0>, <&cru PCLK_CAN0>;   clock-names = "baud", "pclk";
        resets = <&cru SRST_CAN0>, <&cru SRST_P_CAN0>; reset-names = "can", "apb";
        pinctrl-0 = <&can0m0_pins>; };
    can1: can@fea60000  ... GIC_SPI 342 ... CLK_CAN1/PCLK_CAN1 ... SRST_CAN1/SRST_P_CAN1 ... can1m0_pins
    can2: can@fea70000  ... GIC_SPI 343 ... CLK_CAN2/PCLK_CAN2 ... SRST_CAN2/SRST_P_CAN2 ... can2m0_pins

- 补丁链接（v1，PATCH 5/6）：<https://patchew.org/linux/20260630164336.3444550-1-heiko@sntech.de/20260630164336.3444550-6-heiko@sntech.de/>
- 合并后的 binding（`torvalds/linux` master，`Documentation/devicetree/bindings/net/can/rockchip,rk3568v2-canfd.yaml`）把 reset-names 定为 `core`/`apb`，clock-names 仍是 `baud`/`pclk`，compatible 枚举 `rockchip,rk3568v2-canfd` / `rockchip,rk3588-canfd`。
- **注意**：合并版驱动用 `devm_clk_bulk_get_all()` 并按 `clks[0]` 取 baud 时钟（`linux:...-core.c:874-899`），所以 **DT 里 `clocks` 的顺序必须是 baud 在前**，否则 `clk_get_rate()` 拿到 APB 时钟，位时序全错。

### 3.2 时钟（`linux:drivers/clk/rockchip/clk-rk3588.c:1063-1077`）

    PCLK_CAN0  GATE       CLKGATE_CON(11) bit 8      parent pclk_top_root
    CLK_CAN0   COMPOSITE  CLKSEL_CON(39) bit 5 mux (gpll|cpll), div bits 0-4, gate CON(11) bit 9
    PCLK_CAN1  GATE       CON(11) bit 10
    CLK_CAN1   COMPOSITE  CLKSEL_CON(39) bit 11 mux, div bits 6-10, gate CON(11) bit 11
    PCLK_CAN2  GATE       CON(11) bit 12
    CLK_CAN2   COMPOSITE  CLKSEL_CON(40) bit 5 mux, div bits 0-4, gate CON(11) bit 13

- Linux 时钟 ID 号（可直接复用到 Zephyr 头文件）：`PCLK_CAN0=101, CLK_CAN0=102, PCLK_CAN1=103, CLK_CAN1=104, PCLK_CAN2=105, CLK_CAN2=106`（`linux:include/dt-bindings/clock/rockchip,rk3588-cru.h:116-121`）。
- PLL 频率：CPLL 1200 MHz、GPLL 1188 MHz（`linux:drivers/clk/rockchip/clk-rk3588.c:76-77`）。配合 5 bit 分频器：`1200/4=300 MHz`、`1188/12=99 MHz` —— 与票面「默认 99 MHz、建议 300 MHz」在算术上自洽，但 **树内没有任何 Linux DTS 用 assigned-clocks 固定这个值**，所以默认值只能算「TRM/票面提供的经验值」，Zephyr 要么假定 bootloader 留下的值，要么自己写 CLKSEL_CON(39)/(40)。
- **erratum 5 的已知好时钟是 ≥ 300 MHz**：`RKCANFD_ERRATUM_5_SYSCLOCK_HZ_MIN (300*MEGA)`，低于它会打印降级警告（`linux:...-core.c:297,795-800`；合并版 `core.c:809-813`）。99 MHz 下 erratum 5 会「几乎立刻出现」（h:330-332），这决定 Zephyr 首切片必须在 CRU 里把 CAN 时钟拉到 300 MHz，否则空 FIFO workaround 会持续触发、性能不可接受。

### 3.3 复位（`linux:include/dt-bindings/reset/rockchip,rk3588-cru.h:102-107`）

`SRST_P_CAN0=79, SRST_CAN0=80, SRST_P_CAN1=81, SRST_CAN1=82, SRST_P_CAN2=83, SRST_CAN2=84`。
Zephyr 的复位驱动是纯算术映射：`reg = 0xA00 + (id/16)*4`，`bit = id%16`（`zephyr:drivers/reset/reset_rk3588_cru.c:15,39-60`），所以 **只需加 ID 宏，驱动不用改**。这是 Zephyr 侧少数「几乎零成本」的复用点。

### 3.4 引脚（`linux:arch/arm64/boot/dts/rockchip/rk3588-base-pinctrl.dtsi:70-127`）

| 组 | 信号 | bank/pin | func |
|---|---|---|---|
| can0m0 | rx / tx | bank0 PC0 / bank0 PB7 | 11 / 11 |
| can0m1 | rx / tx | bank4 PD5 / bank4 PD4 | 9 / 9 |
| can1m0 | rx / tx | bank3 PB5 / bank3 PB6 | 9 / 9 |
| can1m1 | rx / tx | bank4 PB2 / bank4 PB3 | 12 / 12 |
| can2m0 | rx / tx | bank3 PC4 / bank3 PC5 | 9 / 9 |
| can2m1 | rx / tx | bank0 PD4 / bank0 PD5 | 10 / 10 |

- 全部 `pcfg_pull_none`。
- 这些 mux 位点在 IOC（`bus_ioc@fd5f8000`）里；Linux 的 RK3588 pinctrl 是 4 bit/pin 的 iomux、5 个 bank、含 pull/drive/schmitt 三套寄存器（`linux:drivers/pinctrl/pinctrl-rockchip.c:307-308,5236-5255`）。
- Zephyr 侧 `bus_ioc` 只是 `compatible = "syscon"`（`zephyr:dts/arm64/rockchip/rk3588s-baovm.dtsi:83-86`），`pinctrl` 节点 `compatible = "rockchip,rk3588-pinctrl"`（同文件 :88-93）但 **没有任何驱动**。

---

## 4. 2026 年 RK3588 variant 补丁：时间线与最终状态

### 4.1 v1（2026-06-30，Heiko Stuebner，6 patch）

- 系列首页：<https://patchew.org/linux/20260630164336.3444550-1-heiko@sntech.de/>
- 关键内容：新增 compatible `rockchip,rk3588-canfd`；`rkcanfd_devtype_data_rk3588.quirks = RKCANFD_QUIRK_CANFD_BROKEN`（CAN-FD 关闭）；先只开 erratum 6，再开 erratum 5；RK3588 三路 CAN 节点。
- v1 的判断依据是 commit message：「From the start it does not claim to support can-fd in any part of the documentation, so it seems that is still broken.」

### 4.2 v2（2026-07-02，Cunhao Lu，4 patch）

- 封面：<https://lkml.iu.edu/hypermail/linux/kernel/2607.0/04180.html>
- patch 2/4：<https://lkml.iu.edu/hypermail/linux/kernel/2607.0/04185.html>
- 新增硬件差异：**RK3588 的 `RX_FIFO_CNT` 在 bit 7:5，RK3568 在 6:4**，因此 `rkcanfd_rx_fifo_get_len()` 需要按 model 分支；仍保留 `CANFD_BROKEN`，并记录新的失败模式「BRS 且数据段波特率不同 → 立刻 bus-off」。

### 4.3 v4/v6（2026-07-30 及以后）—— 最终状态

- v6 patch 2/4：<https://lkml.iu.edu/hypermail/linux/kernel/2607.3/13495.html>
- 变更点：TDC 在 RK3588 上写 0；**移除 `CANFD_BROKEN`，开 FD**；测试条件 300 MHz CAN 时钟、500 kbit/s 仲裁、1/3/5 Mbit/s 数据段，5 Mbit/s 跑 15 分钟，两端各 9528377 帧 / 150667356 字节、0 bus-error / 0 bus-off。
- 已合并进 `torvalds/linux` master（2026-10-20 抓取）：
  - `drivers/net/can/rockchip/rockchip_canfd-core.c:53-57` → `quirks = ERRATUM_5 | ERRATUM_6`
  - 同文件 `:159-163` → `/* RK3588 CAN-FD BRS works with TDC disabled. */ ... write(TDC, 0)`
  - `rockchip_canfd-rx.c:284-287` → 按 model 选 `RX_FIFO_CNT_RK3588`(7:5) / `_RK3568`(6:4)
  - `rockchip_canfd.h:217-218` → 两个 GENMASK
  - `:864-865` → of_match 的 `rockchip,rk3588-canfd`
- **未合并**：DTS 节点。master 的 `rk3588-base.dtsi`（99738 B）无 `can@fea5…`，`rk3588-extra.dtsi` 亦无。
- 另有一条并行的稳定性系列（v4，2026-10-15）「can: fix bus-off restart and device close races」，改的是 rockchip 驱动的 restart/close 竞态，与寄存器映射无关，但说明该驱动仍在活跃修 bug：<https://patchew.org/linux/20261015-rockchip-canfd-restart-close-v4-0-ad149ff79450@qq.com/>

### 4.4 对本仓库 Linux 树的影响

本仓库 `linux:` 是 Collabora 的 `rockchip-3588` 7.2 树，**完全没有 RK3588 CANFD**：`of_match` 只有 rk3568v2/v3（`linux:...-core.c:842-852`），`rk3588-base.dtsi` 无 can 节点（已 grep）。所以：

- 参考实现请以 **torvalds master 的 `rockchip_canfd-*.{c,h}`** 为准，而不是本地树；
- 若要在本机 Linux 上验证，需要把 v6 系列 + DTS 补丁打到本地树（本文不做）。

---

## 5. Zephyr 侧：能复用多少

| 需求 | Zephyr 现状 | 结论 |
|---|---|---|
| CAN 子系统 API | 完整，含 FD（`CONFIG_CAN_FD_MODE`，`CAN_MAX_DLEN` 8/64，`zephyr:include/zephyr/drivers/can.h:74-78,101`） | 直接可用 |
| `can_mcan` | Bosch M_CAN 专用（`zephyr:drivers/can/can_mcan.h:20-24`；`Kconfig.mcan` 仅开关后端） | **不可用** |
| 参考驱动骨架 | `can_sja1000.c`（813 行 + 272 行头）是最接近的「单/双滤波器 + 软件过滤」模板；`can_common.c` 578 行提供公共状态回调 | 可抄结构，不可抄寄存器 |
| GIC | GICv3 驱动 + 4 cell DT 编码（`zephyr:dts/arm64/rockchip/rk3588.dtsi:81-90`），板上已有 SPI 422 用例（`zephyr:boards/baovm/lubancat4_vm1/lubancat4_vm1.dts:34-40`） | 341/342/343 可用，无需改驱动 |
| CRU 时钟 | 硬编码 gate 表（`clock_control_rk3588_cru.c:30-82`），`get_rate` 也硬编码（:121-150），**无 set_rate**；ID 头只有 GMAC（`include/zephyr/dt-bindings/clock/rk3588-cru.h:11-19`） | 需加 6 个 ID + 查表项 + 一个「固定 300 MHz」或新增分频编程 |
| CRU 复位 | 算法通用（`reset_rk3588_cru.c:15,39-60`） | 只加 6 个 ID |
| pinctrl | **无 RK3588 驱动**；只有 binding 头（仅 SPI0 M2 四个宏，`include/zephyr/dt-bindings/pinctrl/rockchip-rk3588-pinctrl.h:11-51`）和 `&pinctrl` 节点 | **最大缺口** |
| CAN DT binding | `dts/bindings/can/` 无 rockchip 项 | 新写 |
| 板级基线 | defconfig 引用了 `CONFIG_SPI_ROCKCHIP=y` / `CONFIG_PINCTRL_ROCKCHIP=y`（`boards/baovm/lubancat4_vm1/lubancat4_vm1_defconfig:17,19`），但全树没有这两个符号的定义，`drivers/pinctrl`、`drivers/spi` 下也没有 Rockchip 驱动 | 该分支的 SPI/pinctrl 支持尚未落树；原生 CAN 不能指望它们 |

补充：当前板的 `zephyr,canbus` 指向 **外置 SPI CAN 控制器** `mcp2518fd0`（`boards/baovm/lubancat4_vm1/lubancat4_vm1.dts:22,113-120`），且该节点 `status = "disabled"`。也就是说「原生 CAN」在 Zephyr 侧是从零开始，与现有外置方案并行。

---

## 6. 最小驱动切片（classic CAN）

### 6.1 文件清单

1. `dts/bindings/can/rockchip,rk3588-canfd.yaml` — 新绑定：`compatible`、`reg`、`interrupts`、`clocks`(baud/pclk)、`resets`(core/apb)、`pinctrl-0`、`bus-speed`（Zephyr CAN 绑定惯例，参考 `dts/bindings/can/can-controller.yaml`）。
2. `drivers/can/can_rockchip_canfd.c` — 驱动本体（估 600-800 行）。
3. `drivers/can/Kconfig.rockchip` + `drivers/can/CMakeLists.txt` — `config CAN_ROCKCHIP_CANFD`，`depends on DT_HAS_ROCKCHIP_RK3588_CANFD_ENABLED`，首切片不 select `CAN_FD_MODE`。
4. `dts/arm64/rockchip/rk3588.dtsi` — 追加 can0/1/2 节点（搬 §3.1，改 `interrupts` 为 Zephyr 4 cell 编码 `<GIC_SPI 341 IRQ_TYPE_LEVEL IRQ_DEFAULT_PRIORITY>`）。
5. `include/zephyr/dt-bindings/clock/rk3588-cru.h` — 加 `PCLK_CAN0..2` / `CLK_CAN0..2`（101-106）。
6. `drivers/clock_control/clock_control_rk3588_cru.c` — `rk3588_cru_clk_gate_lookup()` 加 6 项：`CON(11)` bit 8/9、10/11、12/13；`get_rate()` 加 CAN 分支。
7. `include/zephyr/dt-bindings/reset/rk3588-cru-reset.h` — 加 79-84。
8. pinctrl：`include/zephyr/dt-bindings/pinctrl/rockchip-rk3588-pinctrl.h` 加 12 个 `RK_PINMUX()` 宏 + `dts/arm64/rockchip/rk3588-pinctrl.dtsi` 加 can0/1/2 m0/m1 组 + **新写 RK3588 pinctrl 驱动**（见 §6.3）。
9. 测试：`tests/drivers/can/api`（已有）、`tests/drivers/can/timing`（已有）在板上的 build-only 覆盖；外部回环需要硬件。

### 6.2 驱动函数清单（对齐 `zephyr:include/zephyr/drivers/can.h:566-650` 的 `can_driver_api`）

- `rkcanfd_read/write()`
- `rkcanfd_init()`：映射 regs、`clock_control_on(pclk)`、`clock_control_on(baud)`、复位 assert/deassert、`MODE=0`、`RX_FIFO_CTRL|=ENABLE`、建 TX 信号量
- `rkcanfd_set_timing()` / `rkcanfd_set_timing_data()`：brp/2-1、tseg 编码（§2.3）；classic 切片只需 nominal
- `rkcanfd_start()` / `rkcanfd_stop()`：`MODE` 工作/复位模式 + 中断屏蔽
- `rkcanfd_set_mode()`：normal / loopback / silent / listen-only 到 MODE 位的映射
- `rkcanfd_send()`：取 2 深 mailbox 空位（信号量 + `STATE.TX_BUFFER_FULL`），写 FD/classic TX 窗口，写 `CMD`
- `rkcanfd_irq_handler()`：读 INT → 写 1 清除 → RX / ERROR / BUS_OFF / 状态分发（照 §2.4）
- `rkcanfd_rx()`：循环读 `RX_FIFO_CTRL` 计数（**必须用 7:5**）→ 读 3 头字 + 数据 → 空 FIFO 检测（3 字相等）→ 软件过滤 → 回调
- `rkcanfd_add_rx_filter()` / `remove_rx_filter()`：硬件只有一组全局 ID/mask + 5 组扩展 filter（h:221-237），首切片用软件过滤列表（抄 `can_sja1000.c:426-470` 的模式），`get_max_filters()` 返回 0
- `rkcanfd_get_state()` / `get_core_clock()` / `recover()`
- TX 完成：**照抄 RXSTX 自收匹配**（§2.5），或先验证 `TX_FINISH_INT` 行为（Linux 一直屏蔽它，没有证据表明可用）

### 6.3 pinctrl 的现实选项

| 选项 | 工作量 | 可上游性 |
|---|---|---|
| A. 写通用 RK3588 pinctrl 驱动（IOC iomux + pull/drive） | 3-5 人日（500 行 + binding + 测试） | 好，且能同时服务 SPI/GMAC 等 |
| B. 在 CAN 驱动 `init()` 里直接写 `bus_ioc` 的 iomux 位 | 0.5 人日 | 差；维护者通常要求 pinctrl |
| C. 假定 bootloader/pinctrl 已配好，Zephyr 不动引脚 | 0 人日 | 仅 bring-up 阶段可接受 |

> 注意：当前分支的 SPI 也依赖缺失的 pinctrl 驱动（`spi0` 用了 `pinctrl-0 = <&spi0_m2>`，`rk3588s-baovm.dtsi:109-110`），所以选项 A 事实上是「SPI + CAN 一起解锁」的投资。

---

## 7. 与 Linux 的差异 / 不能直接照搬的部分

1. **许可证**：Linux 驱动是 `GPL-2.0`（`linux:...-core.c:1`），Zephyr 是 Apache-2.0。寄存器常量/位域是硬件事实，但**逐行抄 `rockchip_canfd.h` 的注释与 erratum 文本会带来许可风险**。建议：寄存器表按 TRM/合并版 binding 重写，erratum 行为描述自己组织语言；若确需搬运，走 `doc/contribute/guidelines.rst` 的「Components using other Licenses」流程。
2. **RX 完成模型**：Linux 用 NAPI + `can_rx_offload`（`core.c:915`、`rx.c:272`）批量收包；Zephyr 没有 NAPI，必须在 ISR 里直接读 FIFO 并回调/入队，注意 Zephyr 的 CAN 回调在 ISR 上下文。
3. **TX 排队**：Linux 用 `netif_subqueue_maybe_stop` + echo skb（`tx.c:78-89`）；Zephyr 用 `k_sem` 表示 2 个 mailbox 空位 + `can_send()` 超时。
4. **错误状态机**：Linux 用 `can_change_state()`/`can_bus_off()` 与 `struct can_berr_counter`；Zephyr 只有 `can_fire_state_change_callbacks()`（`zephyr:drivers/can/can_common.c:235`），状态判定要自己写。
5. **位时序算法**：Linux 有 `can_calc_bittiming()`；Zephyr 有 `can_calc_timing()`（`can.h:1066`），但要求驱动实现 `get_core_clock()`，且 **Rockchip 的 brp 必须为偶数**（h:78-115 的 ×2 约定），要在 `set_timing()` 里校验/回退。
6. **时钟管理**：Linux 用 `devm_clk_bulk_get_all` + runtime PM（`core.c:766-780,874-878`）；Zephyr 用 `clock_control` + `reset` API，且 CRU 没有 set_rate（§5），必须自己决定 300 MHz 从哪来。
7. **时间戳**：Linux 用 `cyclecounter/timecounter` + delayed work（`timestamp.c`）；Zephyr 无等价框架，首切片建议不做。
8. **erratum 5 的 ≥300 MHz 前提**：Linux 只是打印警告（`core.c:795-800`），Zephyr 若跑 99 MHz 会持续命中空 FIFO 分支。**这是「classic 也不一定稳」的最大技术风险**。

---

## 8. 工作量估计（人日，含自测）

| 项 | 估时 | 说明 |
|---|---|---|
| DT binding + SoC dtsi 节点（can0/1/2） | 0.5-1 | 照 §3.1/§6.1 |
| CRU 时钟 ID + gate 表 + 固定/可编程 300 MHz | 1-2 | 可选：给 CRU 加 `set_rate`（+1-2 天） |
| 复位 ID | 0.1 | 纯宏 |
| pinctrl 驱动（选项 A） | 3-5 | 若无，CAN 引脚无法复用 |
| CAN 驱动 classic（含 IRQ、软件过滤、错误状态） | 3-5 | 参照 `can_sja1000.c` 规模 |
| 板上 bring-up + 双机对测（classic 500k/1M） | 1-2 | 需硬件 |
| 文档 + 上游化（binding 检查、compliance） | 1 | Zephyr CI 要求 |
| **合计（classic，含 pinctrl）** | **8-13** | 不含 FD |
| FD 追加（TDC=0 + data bittiming + `CAN_FD_MODE`） | 1-3 | 依赖硬件验证 TDC=0 |
| **合计（classic + FD）** | **9-16** | |

参照：Zephyr 现有同类驱动规模 `can_sja1000.c` 813 行、`can_mspm0_canfd.c` 336 行、`can_renesas_rz_canfd.c` 1032 行。

---

## 9. 风险与 go/no-go

### 9.1 风险（按严重度）

1. **无硬件在环**：本次研究没有板子、没有逻辑分析仪，所有结论来自代码与补丁；「能收能发」必须由人上板验证。
2. **pinctrl 缺口**（§6.3）：不写驱动就只能写 IOC 寄存器，后者难以上游。这是排期最大不确定性。
3. **99 MHz vs 300 MHz**：erratum 5 在 99 MHz 下几乎必现（h:330-332），而 Zephyr CRU 目前不能设频。若不解决，classic 也可能出现「读空 FIFO / 丢帧」。需要 CRU set_rate 或确认 bootloader 已配 300 MHz。
4. **TX 完成路径未验证**：Linux 依赖 RXSTX 自收（`rx.c:94-189`），Zephyr 若改用 `TX_FINISH_INT` 属于无人验证过的路径；照抄 RXSTX 则要求驱动保存 echo 帧并逐字节比较，代码量与出错面都不小。
5. **许可证**（§7.1）：寄存器头不能直接搬 GPL 文本。
6. **FD 状态易误传**：v1 说 broken、v6 说可用；若文档只写 v1，会与上游现状冲突（§4）。
7. **Bao/VM 侧未知**：CAN 的 SPI 341-343 要能进 VM 才有中断。本仓库 `bao-project` 里没搜到设备/IRQ 直通的现成配置（`grep -rn "devices" bao-hypervisor/src/core/*.c` 无结果），需要单独确认；#7 的结论依赖这一点。
8. **分支基线不完整**：defconfig 引用了不存在的 `CONFIG_SPI_ROCKCHIP`/`CONFIG_PINCTRL_ROCKCHIP`，说明该分支的 Rockchip 外设支持尚未落树；原生 CAN 不能假设「SPI/pinctrl 反正已经有了」。

### 9.2 建议

- **GO**：classic-only 最小切片，顺序 = ① CRU 时钟（先固定 300 MHz）② pinctrl（选项 A 或临时 B）③ CAN 驱动 ④ 板上双机对测 ⑤ 上游化。
- **NO-GO（首切片含 FD）**：FD 追加放到 classic 稳定之后；届时以 v6/合并版为蓝本，先在 Linux 上复现「TDC=0 + 5 Mbit/s 15 分钟」再移植。
- **先决条件**（任一不满足就降级为 bring-up PoC，不进上游流程）：
  1. 拿到板子与第二路 CAN 节点（或回环夹具）；
  2. 确认 CAN 时钟能到 300 MHz（改 CRU 或确认 bootloader）；
  3. 决定 pinctrl 方案（A 还是 B）；
  4. 确认 Bao 能把 SPI 341-343 路由进 VM（若目标形态是 VM）。

---

## 10. 不确定项（明确列出）

1. **RK3588 CAN 时钟复位默认值**：票面给「99 MHz」；我只验证了「CPLL 1200/4=300、GPLL 1188/12=99」与 5 bit 分频器自洽（`clk-rk3588.c:76-77,1065-1067`），**没有 TRM 原文**，也没有任何树内 DTS 固定该值。
2. **上游 DTS 节点状态**：我确认 `torvalds/linux` master 的 `rk3588-base.dtsi` 无 can 节点；但无法排除补丁在 rockchip DTS 分支/next 中（本地 Linux 树亦无）。
3. **合并的确切 commit / 版本**：只能确认 master 文件内容含 RK3588 支持；没有拿到 merge commit 号或首次出现的 tag。
4. **Bao 的设备/IRQ 直通能力**：未验证；`bao-project` 中没找到现成 CAN 配置。
5. **`TX_FINISH_INT` 在 RK3588 上是否可靠**：Linux 始终屏蔽它，无公开验证。
6. **erratum 5 的 300 MHz 阈值是否对 RK3588 同样成立**：Heiko 只实测「降频后空 FIFO 立刻出现」，没有给出 RK3588 的阈值曲线。
7. **Zephyr 是否有下游/闭源 RK3588 CAN 驱动**：GitHub code search 只找到 Linux BSP 里的 `rockchip_canfd.c`，没有 Zephyr 实现；「不存在」是搜索结论，不是证明。
8. **pinctrl 驱动工作量**：3-5 人日是按 Linux 驱动复杂度估的，没有拆到寄存器级。
9. **中断延迟/吞吐**：Zephyr 无 NAPI，100% 中断驱动 + 软件过滤在高负载下的表现未评估。
10. **本分支 `CONFIG_SPI_ROCKCHIP`/`CONFIG_PINCTRL_ROCKCHIP` 的来源**：可能是尚未提交的本地工作或外部 module，无法从树内判断。

---

## 11. 主要来源索引

**Linux 驱动（本地树）**

- `linux:drivers/net/can/rockchip/rockchip_canfd.h`（寄存器、erratum、model）
- `linux:drivers/net/can/rockchip/rockchip_canfd-core.c`（probe/时钟/位时序/中断/状态机）
- `linux:drivers/net/can/rockchip/rockchip_canfd-rx.c`（FIFO、RXSTX、空 FIFO）
- `linux:drivers/net/can/rockchip/rockchip_canfd-tx.c`（mailbox、echo）
- `linux:drivers/net/can/rockchip/rockchip_canfd-timestamp.c`
- `linux:drivers/clk/rockchip/clk-rk3588.c:76-77,1063-1077`
- `linux:include/dt-bindings/clock/rockchip,rk3588-cru.h:116-121`
- `linux:include/dt-bindings/reset/rockchip,rk3588-cru.h:102-107`
- `linux:arch/arm64/boot/dts/rockchip/rk3588-base-pinctrl.dtsi:70-127`
- `linux:arch/arm64/boot/dts/rockchip/rk3568.dtsi:308-345`（节点形状参照）
- `linux:drivers/pinctrl/pinctrl-rockchip.c:307-308,5236-5255`

**2026 补丁系列**

- v1（2026-06-30，6 patch）：<https://patchew.org/linux/20260630164336.3444550-1-heiko@sntech.de/>
- v1 patch 5（DTS 节点）：<https://patchew.org/linux/20260630164336.3444550-1-heiko@sntech.de/20260630164336.3444550-6-heiko@sntech.de/>
- v2 封面（2026-07-02）：<https://lkml.iu.edu/hypermail/linux/kernel/2607.0/04180.html>
- v2 patch 2（RX_FIFO_CNT 7:5）：<https://lkml.iu.edu/hypermail/linux/kernel/2607.0/04185.html>
- v6 patch 2（TDC=0，开 FD，测试数据）：<https://lkml.iu.edu/hypermail/linux/kernel/2607.3/13495.html>
- restart/close 系列 v4（2026-10-15）：<https://patchew.org/linux/20261015-rockchip-canfd-restart-close-v4-0-ad149ff79450@qq.com/>

**合并后的上游文件（torvalds/linux master，2026-10-20 抓取）**

- <https://raw.githubusercontent.com/torvalds/linux/master/drivers/net/can/rockchip/rockchip_canfd-core.c>
- <https://raw.githubusercontent.com/torvalds/linux/master/drivers/net/can/rockchip/rockchip_canfd-rx.c>
- <https://raw.githubusercontent.com/torvalds/linux/master/drivers/net/can/rockchip/rockchip_canfd.h>
- <https://raw.githubusercontent.com/torvalds/linux/master/Documentation/devicetree/bindings/net/can/rockchip,rk3568v2-canfd.yaml>
- <https://raw.githubusercontent.com/torvalds/linux/master/arch/arm64/boot/dts/rockchip/rk3588-base.dtsi>（无 can 节点）

**Zephyr**

- `zephyr:include/zephyr/drivers/can.h:74-78,101,566-650,1066`
- `zephyr:drivers/can/can_sja1000.c:426-470`（软件过滤模板）
- `zephyr:drivers/can/can_mcan.h:20-24`（M_CAN 专用）
- `zephyr:drivers/clock_control/clock_control_rk3588_cru.c:20-163`
- `zephyr:drivers/reset/reset_rk3588_cru.c:15-60`
- `zephyr:include/zephyr/dt-bindings/clock/rk3588-cru.h:11-19`
- `zephyr:include/zephyr/dt-bindings/reset/rk3588-cru-reset.h:11-12`
- `zephyr:include/zephyr/dt-bindings/pinctrl/rockchip-rk3588-pinctrl.h:11-51`
- `zephyr:dts/arm64/rockchip/rk3588.dtsi:81-90,118-134`
- `zephyr:dts/arm64/rockchip/rk3588s-baovm.dtsi:72-93`
- `zephyr:boards/baovm/lubancat4_vm1/lubancat4_vm1.dts:22,113-120`
- `zephyr:boards/baovm/lubancat4_vm1/lubancat4_vm1_defconfig:17,19`
