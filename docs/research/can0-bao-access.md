<!--
SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
SPDX-License-Identifier: Apache-2.0
-->

# RK3588 CAN0 在 Bao 下的透传面（C / D 档）

- 对应 issue：[irenebus/zephyr#9](https://github.com/irenebus/zephyr/issues/9)「定最小验收门槛与测试工装」。本文件回答 #9 追加的 Bao 透传问题：C 档（内部 loopback，不需要引脚）与 D 档（两节点，需要 pinmux）。
- 日期：2026-10-20。方式：只读。未改 issue/地图/代码，未 commit。
- 路径约定：`zephyr:` = 本仓库；`linux:` = `../../linux`（RK3588 7.2 树）；`bao:` = `../../bao-project/bao-hypervisor`；`trm:` = RK3588 TRM Part1/Part2（链接见 §8）。为控制篇幅，正文用 `linux:...<basename>:<lines>` 省略中间目录；完整路径见 §8。
- 结论边界：本文只给 Bao 的物理地址/大小/中断与 CRU 访问策略；CAN 驱动、DT binding、pinctrl 驱动仍是 Zephyr 侧工作（见 `zephyr:docs/research/rk3588-native-can.md`）。

---

## 0. 结论摘要（TL;DR）

1. **C 档 Bao 最小集 = CAN0 `0xfea50000/0x1000` + `.interrupts = {373}`。** CRU `0xfd7c0000/0x1000` 是推荐项：loopback 的数据路径不需要它，但驱动要写 gate/mux/reset 时必须有；若走“纯复位默认值”捷径可不映射（§5）。
2. **Bao 的 `.interrupts` 是 GIC INTID，不是 DT 的 SPI 号。** DT `GIC_SPI 341` → Zephyr IRQ/INTID `341 + 32 = 373`；Bao 填 `341` 会挂到错误的虚拟中断。TRM INTID 373 = `irq_can0`；Zephyr `gen_defines.py` 对 GIC_SPI 加 32；板级 DTS 注释也给了同一规则（333→365、422→454、420→452）。
3. **CRU 只需要第一页 4 KiB，不需要 Linux 的 `0x5c000`。** CAN0 用到的三个寄存器都在 `0x000–0xfff`：`CLKSEL_CON39 = 0x39c`、`CLKGATE_CON11 = 0x82c`、`SOFTRST_CON11 = 0xa2c`。Linux 的 `0x5c000` 是整组 CRU（含 PHPTOP/SECURE/PMU CRU），CAN0 不碰。
4. **同一个 CRU 物理页可以同时映射给 Linux VM 和 Zephyr VM。** Bao 的 `devs[]` 只是 per-VM 的 stage-2 映射；没有 MMIO 独占检查，也没有 IOMMU 对 CPU MMIO 的检查。**唯一独占的是中断**：SPI 373 不能同时分给两个 VM。
5. **写 CRU 必须用 bits 31:16 的 write-enable（hiword mask），只改 CAN0 位。** RK3588 的 CLKSEL/GATE/SOFTRST 都支持该机制；Linux 和 Zephyr 现有驱动都已在用。mask 能防“同寄存器不同位的读改写丢失”，但防不了 Linux 改父 PLL 速率（§4.2）。
6. **TRM 复位默认值：CAN0 时钟开、复位已解、99 MHz。** `CRU_GATE_CON11` 复位 `0x0`（bit8/9=0，时钟使能）、`CRU_SOFTRST_CON11` 复位 `0x0`（bit8/9=0，解复位）、`CRU_CLKSEL_CON39` 复位 `0x2CB`（CAN0 mux=GPLL、div=12；GPLL=1188 MHz → 99 MHz）。
7. **“不映射 CRU”在当前部署可跑，但不是契约。** U-Boot mainline/vendor 都不碰 CAN0；本板 Linux cmdline 已带 `clk_ignore_unused`，所以 Linux 不会在 late_initcall 关掉未用时钟。一旦去掉该参数、或 Linux 以后加了 CAN0 节点，这条路就断。
8. **99 MHz 必须带 erratum 5 的空 FIFO workaround。** Linux 的阈值是 300 MHz；99 MHz 下不实现该 workaround，C 档也可能读错/丢帧。300 MHz 是后续优化：写 `CLKSEL_CON39`（CPLL/4=300 需确认 CPLL=1200；GPLL/3=396 需确认 IP 上限）。
9. **C 档 loopback 确实不需要 pinctrl/IOC。** 控制器置 `MODE` 的 `LBACK|SILENT|SELF_TEST`，`SILENT` 关闭 TX 引脚，RX 走内部回环；与 IOC/pinctrl 无关。时钟/复位仍是另一回事（默认值或 CRU）。
10. **D 档必须加 IOC，而且这块板有真实引脚冲突。** CAN0 m0 的 TX=GPIO0_B7 与 PWM0 风扇同脚；m1 的 TX/RX=GPIO4_PD4/PD5 与 SDMMC CLK/CMD 同脚。m0 还需要 `PMU2_IOC(0xfd5f4000)` + `BUS_IOC(0xfd5f8000)`，板级现有的 `bus_ioc/0x3000` 不够。
11. **修正一个会写错寄存器的坑：Zephyr reset ID 是 `reg*16+bit` 编码。** CAN0 应用 `184/185`（`SRST_P_CAN0/SRST_CAN0`），不是 Linux binding 的 `79/80`。现有 Zephyr reset 驱动按 `id/16`、`id%16` 算偏移；79 会写到 `0xa10` 的 bit15，80 会写到 `0xa14` 的 bit0。

上面每条在 §2–§5 展开并附 primary source 行号；§8 是完整索引。

---

## 1. 票面背景与 C/D 分档

#9 已定：验收档位 C（内部 loopback，can0 m0，不需要引脚）、载体 `tests/drivers/can/api` + `can_shell`、构建走 Zephyr SDK + `west build`；并留下“待确认：CAN0 + CRU + GIC IRQ 373 的 Bao 透传；C 档 loopback 不依赖引脚，pinctrl/IOC 可跳过；D 档再补 IOC `0xfd5f0000`”。本文就是把这个“待确认”落成地址/大小/中断清单。

C 与 D 的差别只在**数据路径**：C 用控制器内部回环，不需要引脚；D 要真正上总线，需要 IOC pinmux、收发器、终端电阻和第二节点。两者对 Bao 的 CAN0/CRU/IRQ 需求相同，D 额外要 IOC。

---

## 2. C 档清单（loopback，不需要引脚）

| 区域 | 物理地址 | 大小 | IRQ（GIC INTID） | 必要性 |
|---|---|---|---|---|
| CAN0 控制器 | `0xfea50000` | `0x1000` | `373` | 必须 |
| CRU（TOP CRU 第一页） | `0xfd7c0000` | `0x1000` | 无 | 推荐；不映射时见 §5 的捷径 |
| IOC / pinctrl | — | — | — | **不需要**（loopback 不走引脚） |

- CAN0 的寄存器窗口在 TRM 里是 64 KiB（`FEA50000`），但驱动只用一个 4 KiB 页：Linux 的 v1 DTS 补丁和 Zephyr 预期节点都用 `0x1000`（节点形状见 `zephyr:docs/research/rk3588-native-can.md` §3.1；本地 Linux 7.2 树还没有该节点）。
- CRU 页里只需要三个寄存器；三者都小于 `0x1000`，所以 `0x1000` 足够（`trm:Part1` CRU 章）：
  - `CRU_CLKSEL_CON39`：`0x39c`，CAN0 div bits 4:0、mux bit 5（`trm:Part1` p.73）。
  - `CRU_GATE_CON11`：`0x82c`，`pclk_can0_en` bit 8、`clk_can0_en` bit 9（`trm:Part1` p.120）。
  - `CRU_SOFTRST_CON11`：`0xa2c`，`presetn_can0` bit 8、`resetn_can0` bit 9（`trm:Part1` p.172）。
- 若沿用 Zephyr 现有 CRU 节点（`zephyr:dts/arm64/rockchip/rk3588.dtsi:118-134`，reg `0xfd7c0000` 长度 `0x5c000`），**且现有 Bao config 尚未映射 CRU**，新增条目可只映射 `0x1000`：驱动只会访问上述三个偏移；但更干净的做法是把 Zephyr VM 的 CRU `reg` 缩到 `0x1000`，否则 DT 声明与 stage-2 权限不一致，未来谁碰高偏移谁 fault。若现有 config 已经映射了整块 CRU，不要再加一条同 VA 的 `0x1000`（同一 VM 的 VA 重叠会让 `mem_alloc_vpage` 失败），沿用它即可。
- `PCLK_CAN0`/CLK_CAN0 的 gate 位就是 CON11 的 bit8/9（`linux:drivers/clk/rockchip/clk-rk3588.c:1063-1077`）；复位位是 SOFTRST_CON11 的 bit8/9（`linux:drivers/clk/rockchip/rst-rk3588.c:127-132`）。这两组位与 CAN1/CAN2 相邻，写的时候必须 mask（§4.2）。

---

## 3. D 档增量（两节点，需要 pinmux）

D 档在 C 档的基础上增加 IOC，并且要处理本板的引脚复用冲突。

### 3.1 两张 IOC 页面

| 区域 | 物理地址 | 大小 | 说明 |
|---|---|---|---|
| CAN0 m0 用 | `0xfd5f0000` | `0x10000` | 推荐整块映射：PMU1 `0x0`、PMU2 `0x4000`、BUS_IOC `0x8000`、VCCIO `0x9000-0xe000` 都在内；与 Linux 的 `ioc@fd5f0000` 一致 |
| 或拆成两块 | `0xfd5f4000/0x1000` + `0xfd5f8000/0x1000` | — | 只给 CAN0 m0 时够用；但 Zephyr 侧 pinctrl 节点的基址要相应处理 |
| 板级现有 `bus_ioc` | `0xfd5f8000` | `0x3000` | 覆盖 BUS_IOC/VCCIO，**不覆盖 PMU2 `0x4000`**，所以单独用它做 CAN0 m0 不够 |

- CAN0 m0：TX = GPIO0_B7（func 11），RX = GPIO0_C0（func 11）。最终功能位在 BUS_IOC：
  - `BUS_IOC_GPIO0B_IOMUX_SEL_H[15:12] = 0xb`（`trm:Part1` p.985 寄存器细节，offset 0x000C；p.1428 引脚复用表也列出 `CAN0_TX_M0`）。
  - `BUS_IOC_GPIO0C_IOMUX_SEL_L[3:0] = 0xb`（`trm:Part1` p.985，offset 0x0010；`CAN0_RX_M0`）。
- 但 bank0 的 PB4–PD7 是“双段 mux”：Linux pinctrl 先写 PMU2 的 `8`（把引脚路由到 BUS_IOC），再写 BUS_IOC 的 `0xb`（`linux:drivers/pinctrl/pinctrl-rockchip.c:1356-1390`；读回逻辑在 `:1227-1248`）。所以只映射 `bus_ioc` 而拿到 PMU2 的 `0x4000` 页，CAN0 m0 的 mux 不完整。
- Linux 的 pinctrl `rockchip,grf` 指向 `ioc@fd5f0000`（`linux:arch/arm64/boot/dts/rockchip/rk3588-base.dtsi:3538-3542`），并在基址上加 `0x4000`/`0x8000`。本板 Zephyr 的 `pinctrl` 节点却把 `rockchip,grf` 指向 `bus_ioc@fd5f8000`（`zephyr:dts/arm64/rockchip/rk3588s-baovm.dtsi:83-93`）。如果将来 Zephyr pinctrl 驱动照抄 Linux 的“基址 + 0x8000”逻辑，会二次偏移。D 档开始前必须二选一：把 Zephyr 的 pinctrl 基址改成 `ioc@fd5f0000`，或让驱动明确知道自己拿到的是 `bus_ioc` 基址。

### 3.2 本板引脚冲突（必须先做板级决定）

| 复用 | 引脚 | 板上用途 | 影响 |
|---|---|---|---|
| CAN0 m0 TX | GPIO0_B7，func 11 | **PWM0 风扇**：`pwm0m0_pins` 用同一脚 func 3；`&pwm0 { status = "okay"; }`，`fan` 用它（`linux:...rk3588s-lubancat4-baovm.dts:77-81,840-842`；`linux:...rk3588-base-pinctrl.dtsi:2205-2209`） | 用 m0 就必须关风扇（短测可接受，长期有热风险） |
| CAN0 m1 TX/RX | GPIO4_PD4/PD5，func 9 | **SDMMC CLK/CMD**：`sdmmc_clk/cmd` 用同脚 func 1（`linux:...rk3588-base-pinctrl.dtsi:2650-2668`）；本板 `aliases { mmc0 = &sdmmc; }` 且 root 在 `mmcblk0p2` | **不能用 m1**，会打断 SD/root |
| CAN1 m0 TX/RX | GPIO3_B5/B6，func 9（`linux:...rk3588-base-pinctrl.dtsi:92-98`） | 本板 DTS 自身未见 GPIO3_B5/B6 被占用（LED/SBU 用的是 GPIO4_B3/B4/B5）；40-pin 是否引出需查原理图 | 可选第二控制器；只需 `bus_ioc`，不需要 PMU2 |

- 所以 D 档的现实选择是 **CAN0 m0 + 关 `&pwm0` 风扇**，或者用 **CAN1 m0** 做第二个控制器（若 40-pin 可用），或者回到板上已有的 SPI-CAN（MCP2518FD）。不要把 m1 当作“避开风扇”的方案：m1 撞的是 SD。
- D 档还需要 Zephyr 侧的 **pinctrl 驱动**、CAN0 m0/m1 的 pinmux 宏、以及 `rockchip,grf` 基址修正；这些不在 Bao 配置里，但缺了 D 档跑不起来（`zephyr:include/zephyr/dt-bindings/pinctrl/rockchip-rk3588-pinctrl.h:11-51` 目前只有 SPI0 M2 宏；`drivers/pinctrl/` 无 Rockchip 驱动）。
- 两节点的总线侧（收发器、120 Ω、第二节点/USB-CAN）是板级/工装问题，#9 已覆盖；Bao 只需要把 CAN0 的寄存器和 IRQ 给 Zephyr VM。

### 3.3 可选：第二控制器 CAN1

如果第二节点走同一块板的 CAN1，而不是外接 USB-CAN：

| 区域 | 物理地址 | 大小 | IRQ | 备注 |
|---|---|---|---|---|
| CAN1 | `0xfea60000` | `0x1000` | `374`（SPI 342 + 32） | 同一 CRU 页的 `CLKSEL_CON39` bits 6-11、`GATE_CON11` bits 10/11、`SOFTRST_CON11` bits 10/11 |
| CAN1 m0 IOC | `0xfd5f8000`（已在 `bus_ioc/0x3000` 内） | — | — | bank3，不需要 PMU2 |

CAN2（`0xfea70000`、IRQ 375）同理，但本板没有明显空闲引脚，不建议作为 D 档首选。

---

## 4. 同一 CRU 页共享给两个 VM：Bao 允许，但只有 IRQ 是独占的

### 4.1 Bao 没有 MMIO 独占检查

- `struct vm_dev_region` 只有 `pa/va/size/interrupt_num/interrupts/id`，没有 owner/refcount（`bao:src/core/inc/vm.h:33-40`）。
- `vm_init_dev()` 对每个 VM 的每个 dev 调 `mem_alloc_map_dev()`；后者只在该 VM 的 `addr_space` 里分配 VA 并写自己的 stage-2 页表（`bao:src/core/vm.c:209-226`、`bao:src/core/mmu/mem.c:968-979`）。`mem_alloc_vpage()` 只检查**本 VM 地址空间**的 VA 段，不查物理页是否已被别的 VM 映射（`bao:src/core/mmu/mem.c:309` 起）。
- `mem_init_reserved()` 只预留 VM image、`place_phys` 的 `regions` 和 shmem；**不预留 devs 的物理页**（`bao:src/core/mem.c:319-445`）。所以 CRU 不会因为“已被 Linux 映射”而被 Bao 拒绝。
- IOMMU 也不是 MMIO 所有权检查：`io_vm_init()` 只在 `platform.arch.smmu.base` 存在时初始化 SMMU；`dev->id` 是 DMA 的 stream ID，只有非 0 才加入 SMMU（`bao:src/arch/armv8/armv8-a/iommu.c:12-20,72-88`、`bao:src/core/vm.c:228-237`）。CAN0 不做 DMA，CRU 也不是 bus master，保持 `.id = 0` 即可。没有 SMMU 时，CPU 对 CRU 的访问完全由 stage-2 决定。
- 中断是独占的：`interrupts_vm_assign()` 查全局 bitmap，SPI（INTID > 31）重复就返回 false，`vm_init_dev()` 直接 `ERROR`（`bao:src/core/interrupts.c:111-126`、`bao:src/arch/armv8/interrupts.c:51-65`、`bao:src/core/vm.c:221-225`）。所以 **373 只能出现在 Zephyr VM 的 `devs[]` 里**；Linux VM 的配置里不能有 373。

### 4.2 写 CRU 的正确姿势：hiword write-enable，只碰 CAN0 位

RK3588 的 CRU 寄存器低 16 位是数据，高 16 位是逐位 write-enable：写 `bit n` 时同时把 `bit 16+n` 置 1，只有被 enable 的位会更新（`trm:Part1` CLKSEL_CON39 p.73、GATE_CON11 p.120、SOFTRST_CON11 p.172 的 “write_enable / Write enable for lower 16 bits, each bit is individual”）。Linux 的 `HIWORD_UPDATE(val, mask, shift)` 就是 `(val << shift) | (mask << (shift + 16))`（`linux:drivers/clk/rockchip/clk.h:26`），RK3588 的 mux/div/gate 全部带 HIWORD 标志（`linux:drivers/clk/rockchip/clk-rk3588.c:522-524`），reset 走 LUT + `ROCKCHIP_SOFTRST_HIWORD_MASK`（`linux:drivers/clk/rockchip/rst-rk3588.c:14-19,853-857`）。

Zephyr 现有驱动已经采用同一机制：

- gate：`(BIT(shift) << 16) | (reg & ~BIT(shift))`（`zephyr:drivers/clock_control/clock_control_rk3588_cru.c:74-86`）。
- reset：`(BIT(bit) << 16) | (reg | BIT(bit))` / `(BIT(bit) << 16) | (reg & ~BIT(bit))`（`zephyr:drivers/reset/reset_rk3588_cru.c:25-37`）。

因此：

- 写 `CLKGATE_CON11` 只动 bit8/9（CAN0），不会碰 bit10/11（CAN1）、bit12/13（CAN2）。
- 写 `SOFTRST_CON11` 只动 bit8/9；即使 Linux 同时改 CAN1/CAN2 的 reset，两边的 mask 也不会互相覆盖。
- 写 `CLKSEL_CON39` 必须用 mask `0x3f`（bit0-5），不能 `sys_write32` 整个寄存器；否则会踩 CAN1 的 bits 6-11。
- 新增 `set_rate` 时用 `(0x3f << 16) | (mux << 5) | div`。300 MHz 的例子：mux=CPLL(1)、div=3，即 `(0x3f << 16) | (1 << 5) | 3`（前提 CPLL=1200 MHz；见 §7 不确定项）。

**残留风险**：hiword mask 只保证“同寄存器不同位的写不丢”，不保证“Linux 不改父 PLL”。CAN0 的父时钟是 `gpll_cpll_p`（`linux:...clk-rk3588.c:1065-1067`）；如果 Linux 在运行期改 GPLL/CPLL 频率，CAN0 的实际 baud 会跟着变。当前 Linux 没有 CAN0 consumer，GPLL 由 `assigned-clock-rates` 固定为 1188 MHz（`linux:...rk3588-base.dtsi:748-753`），风险低；但 Zephyr 的 `get_rate` 若硬编码 99 MHz，仍应在文档/代码里标注“前提是 GPLL 不变”。更稳的做法是让 CAN0 时钟子树单 owner（Linux 不注册 CAN0 consumer），并把实际父频率读出来。

### 4.3 reset ID 的编码修正（容易写错）

Zephyr 的 `rk3588-cru-reset.h` 不是 Linux binding 的 ID，而是 `reg*16 + bit` 的编码：`SRST_A_GMAC0 = 522 = 32*16 + 10`，对应 Linux 的 reg 32 bit 10（`zephyr:include/zephyr/dt-bindings/reset/rk3588-cru-reset.h:11-12`、`linux:drivers/clk/rockchip/rst-rk3588.c:381-382`）。所以 CAN0：

- `SRST_P_CAN0`（Linux 79）= reg 11 bit 8 → Zephyr ID **184**。
- `SRST_CAN0`（Linux 80）= reg 11 bit 9 → Zephyr ID **185**。

`reset_rk3588_cru.c` 的 `0xa00 + (id/16)*4`、`id%16` 会把 184/185 算成 `0xa2c` 的 bit8/9，正确；把 Linux 的 79/80 直接抄进来会算成 `0xa10` bit15（79）和 `0xa14` bit0（80），是错的。CAN 时钟 ID 则可以照抄 Linux 的 `PCLK_CAN0=101`/`CLK_CAN0=102`，因为 clock 驱动是按 ID 查表，不做算术（`zephyr:drivers/clock_control/clock_control_rk3588_cru.c:30-72`）。

---

## 5. 能不能不映射 CRU，靠复位默认值/U-Boot？

### 5.1 硬件默认值（TRM，均为 CRU 复位值）

| 寄存器 | 地址 | 复位值 | 对 CAN0 的含义 |
|---|---|---|---|
| `CRU_CLKSEL_CON39` | `0x39c` | `0x000002CB` | CAN0 sel=0（GPLL），div=0xb（/12）→ GPLL 1188/12 = **99 MHz**；CAN1 同样 |
| `CRU_GATE_CON11` | `0x82c` | `0x00000000` | bit8 `pclk_can0_en`=0、bit9 `clk_can0_en`=0；“When high, disable clock” → **时钟开** |
| `CRU_SOFTRST_CON11` | `0xa2c` | `0x00000000` | bit8 `presetn_can0`=0、bit9 `resetn_can0`=0；“When high, reset” → **已解复位** |

来源：`trm:Part1` CRU 章，CLKSEL_CON39 p.73、GATE_CON11 p.120、SOFTRST_CON11 p.172。TRM 的 PLL 寄存器本身复位为 0，GPLL=1188 MHz 不是“硬件复位常量”，而是正常启动链（U-Boot 的 `rk3588_clk_init` + Linux 的 `assigned-clock-rates`）设的；见 5.2。

### 5.2 U-Boot 会不会配 CAN0？

- **Mainline U-Boot**：`drivers/clk/rockchip/clk_rk3588.c` 的 `rk3588_clk_probe()` 只设 PLL、处理 DT 的 `assigned-clocks`，没有 CAN 的 gate/rate 分支（`https://github.com/u-boot/u-boot/blob/master/drivers/clk/rockchip/clk_rk3588.c`，probe 在 1959-1995 行；全文件没有 `CLK_CAN*` 表项）。
- **Vendor U-Boot**（`rockchip-linux/u-boot`，`next-dev`）：同一文件对 `can` 的大小写不敏感匹配为 0，同样不碰 CAN0（`https://github.com/rockchip-linux/u-boot/blob/next-dev/drivers/clk/rockchip/clk_rk3588.c`）。
- 所以 U-Boot 不 enable、不 reset、不配 pin，CAN0 继承 5.1 的复位值。U-Boot 的 `rk3588_clk_init()` 会显式把 CPLL/GPLL 设到 `CPLL_HZ`/`GPLL_HZ`（`clk_rk3588.c:1921-1942`），并处理 DT 的 `assigned-clocks`（probe 1987-1992 行）；Linux 的 CRU 节点又明确把 GPLL 设为 1188 MHz（`linux:...rk3588-base.dtsi:748-753`）。所以正常启动链后 GPLL=1188。
- 未验证的是 BootROM 是否在更早阶段改过 CLKSEL/GATE；没有证据表明它会碰 CAN0。把它列为不确定项（§7），不当作保证。

### 5.3 Linux 会不会把 CAN0 时钟关掉？

会——如果让它跑默认的 `clk_disable_unused()`。Linux 在 `late_initcall_sync` 遍历所有 clock，对 `enable_count==0` 且没有 `CLK_IGNORE_UNUSED` 的 gate 调 `.disable`（`linux:drivers/clk/clk.c:1470-1509,1558`）；CAN0 的 `PCLK_CAN0`/`CLK_CAN0` 没有 critical/ignore 标志，也没有 consumer，所以会被关。

但**本板 Linux VM 的 cmdline 已经带了 `clk_ignore_unused`**（`linux:arch/arm64/boot/dts/rockchip/rk3588s-lubancat4-baovm.dts:22`：`... cpuidle.off=1 clk_ignore_unused`）。`clk_disable_unused()` 看到该参数直接返回（`linux:drivers/clk/clk.c:1519-1526`），所以当前部署下 CAN0 的 gate 不会被 Linux 关。

### 5.4 结论与最小替代方案

| 方案 | 可行性 | 代价/风险 |
|---|---|---|
| **A. 映射 CRU 4 KiB，Zephyr 自己 gate/reset（推荐）** | 可行；Bao 允许共享页；hiword mask 安全 | 需要 Zephyr CRU 驱动加 CAN ID + CAN 驱动调用 clock/reset；无额外风险 |
| **B. 不映射 CRU，吃复位默认值 + `clk_ignore_unused`** | 当前部署可行（99 MHz，gate 开，reset 解；且没有其他 Zephyr consumer 访问 CRU） | 依赖 cmdline 不被改、Linux 不加 CAN0 节点、BootROM/U-Boot 不改 CRU；任何一条破了就“没时钟/被复位”，且无法从 Zephyr 恢复。只适合 C 档 bring-up |
| **C. Bao 预置 CRU** | 理论可行，但本 checkout 的 Bao 没有 `src/platform/rk3588` 平台端口（`ls bao:src/platform` 无 rk3588；`configs/` 只有 example/null），预置代码没有落点 | 需要先有 RK3588 平台端口；且 Linux 仍可能覆盖 |
| **D. 接受 99 MHz + erratum 5 workaround** | 与 A/B 正交；不映射 CRU 时必然 99 MHz | 驱动必须实现空 FIFO 检测；性能/可靠性不如 300 MHz，但 C 档功能验收可过 |

**建议**：C 档验收按 **A + 99 MHz + erratum 5 workaround** 做；如果时间紧，可先用 **B** 证明控制器活着，但验收记录里必须写明“依赖 `clk_ignore_unused`，不是稳定契约”。D 档/后续一律用 A，并在需要时把 `CLKSEL_CON39` 拉到 300 MHz。C 档不要求 300 MHz；300 MHz 是 D/性能项，不是 C 的判据。

---

## 6. 可粘贴的 Bao `devs[]`

配置位置是 Zephyr VM（VM1）的 `.platform`；语法照 `bao:configs/example/config.c:69-79`。下面的片段是**追加项**，不是完整数组：把条目并进现有 `devs[]`，并把 `.dev_num` 加上相应条数。`.va` 一律等于 `.pa`（Zephyr 的设备 MMIO 用物理地址，stage-1 恒等映射；Bao stage-2 也按同一 VA 映射）。`.id` 保持 0（CAN/CRU 都不是 DMA master）。

### 6.1 C 档最小（CAN0 + CRU）

```c
/* 追加到 Zephyr VM 的现有 devs[]；.dev_num += 2（若 CRU 尚未映射） */
    {
        /* CAN0 controller: 0xfea50000, 4 KiB */
        .pa = 0xfea50000,
        .va = 0xfea50000,
        .size = 0x1000,
        .interrupt_num = 1,
        .interrupts = (irqid_t[]) { 373 },   /* GIC INTID, not DT SPI 341 */
    },
    {
        /* TOP CRU first page: CLKSEL_CON39 0x39c,
         * CLKGATE_CON11 0x82c, SOFTRST_CON11 0xa2c. 4 KiB is enough. */
        .pa = 0xfd7c0000,
        .va = 0xfd7c0000,
        .size = 0x1000,
    },
```

如果走 §5.4 的 B 方案（不映射 CRU），删掉第二个 dev，并让 Zephyr 的 CAN 节点不引用 `clocks`/`resets`（或在驱动里跳过 CRU 访问）；否则 CRU 驱动一写就 stage-2 fault。如果现有 config 已经映射了 CRU（例如 GMAC 用），**不要**再加第二个 dev，直接复用。

### 6.2 D 档增量（CAN0 m0 + IOC）

在 6.1 基础上加一个 IOC dev；如果第二节点是 CAN1，再加 CAN1。**注意**：若现有 config 已映射 `bus_ioc@fd5f8000/0x3000`，下面的 `0xfd5f0000/0x10000` 会与它 VA 重叠；请用整块替换旧的 `bus_ioc` 条目，或改成下面“拆成两块”的非重叠写法。

```c
    {
        /* IOC: PMU1 0x0, PMU2 0x4000, BUS_IOC 0x8000, VCCIO 0x9000+.
         * CAN0 m0 needs both PMU2 (route) and BUS_IOC (func 11).
         * This is the same 64 KiB region Linux calls ioc@fd5f0000. */
        .pa = 0xfd5f0000,
        .va = 0xfd5f0000,
        .size = 0x10000,
    },
    {
        /* Optional: CAN1, only if the second node is on this board.
         * CAN1 m0 is bank3 -> BUS_IOC only; IRQ = SPI 342 + 32 = 374. */
        .pa = 0xfea60000,
        .va = 0xfea60000,
        .size = 0x1000,
        .interrupt_num = 1,
        .interrupts = (irqid_t[]) { 374 },
    },
```

如果只想给 CAN0 m0 最小权限，可把 64 KiB 拆成：

```c
    { .pa = 0xfd5f4000, .va = 0xfd5f4000, .size = 0x1000 }, /* PMU2, bank0 route */
    { .pa = 0xfd5f8000, .va = 0xfd5f8000, .size = 0x1000 }, /* BUS_IOC, func 11 */
```

但 Zephyr 的 pinctrl 节点目前是 `bus_ioc@fd5f8000/0x3000`（`zephyr:dts/arm64/rockchip/rk3588s-baovm.dtsi:83-86`），驱动若按 Linux 的“基址 + 0x4000/0x8000”实现，拆块映射会二次偏移；整块 64 KiB 最省事，也最接近 Linux。D 档的 pinmux 软件前提见 §3.2。

---

## 7. 不确定项 / 未验证

1. **TRM 来源**：本文用的是 `http://www.scs.stanford.edu/~zyedidia/docs/rockchip/rk3588_part1.pdf` 的镜像（RK3588 TRM-Part1，Copyright 2022）。未从 Rockchip 官方站点逐页核对；页码以该 PDF 为准。
2. **BootROM 行为**：5.1 是 CRU 复位值；BootROM 是否在 U-Boot 前改过 CLKSEL/GATE/SOFTRST 未验证。没有证据它会碰 CAN0，但“纯复位默认”不能当成跨启动链的保证。
3. **CPLL 实际频率**：300 MHz 方案用 CPLL/4，前提 CPLL=1200 MHz。Linux 的 PLL 表里有 1200 MHz 这一档（`linux:drivers/clk/rockchip/clk-rk3588.c:76`），但 CRU 节点的 `assigned-clock-rates` 没有显式设 CPLL；U-Boot/ROM 的当前值未读。用之前要么读 `CRU_CPLL_CON0/1` 算，要么在 Linux 侧确认。
4. **CAN IP 的最高时钟**：GPLL/3=396 MHz 也 >300 MHz 的 erratum 阈值，但 IP 是否允许 396 未从 TRM/数据手册确认；未确认前不要用它替代 300。
5. **GPLL 运行期变化**：如果 Linux 的某个 consumer 改 GPLL，CAN0 的实际 baud 会变；hiword mask 不防这个。当前没有 CAN0 consumer，风险低，但 `get_rate` 硬编码 99 MHz 时应标注前提。
6. **本板 CAN0 m1 的 40-pin 可用性**：m1 的 PD4/PD5 已确认被 SDMMC 占用，不能用作 CAN；如果改板/换 DTS 释放 SD，才谈得上 m1。CAN1 m0 的 GPIO3_B5/B6 是否引出到 40-pin，需要原理图确认。
7. **Bao 的 RK3588 平台端口**：本 checkout 的 `src/platform` 没有 rk3588，`configs/` 只有 example/null；实际板子的 Bao 配置不在这棵树里（`bin/bao` 是预构建）。本文的地址/IRQ 结论不依赖平台端口，但“把 snippet 放到哪个 config.c”需要板子的部署树。
8. **硬件在环**：本次没有板子、没有逻辑分析仪；所有结论来自 TRM、Linux/Bao/Zephyr 源码。C/D 档的“能收能发”必须由人上板验证。
9. **Linux VM 的 CAN0 节点**：本地 7.2 树的 `rk3588s-lubancat4-baovm.dts` 没有 CAN0 节点，所以当前没有 Linux consumer；如果以后合并上游 DTS 的 CAN0 节点，必须在该 VM 里 `status = "disabled"`，否则 Linux 会抢 373/时钟/reset。

---

## 8. 主要来源索引

**TRM（RK3588 TRM-Part1/Part2）**

- Part1：`http://www.scs.stanford.edu/~zyedidia/docs/rockchip/rk3588_part1.pdf`
  - Ch.1 存储映射：CAN0 `FEA50000`，64 KiB。
  - Ch.2 中断表：INTID `373 = irq_can0`（p.23-24）。
  - CRU：`CRU_CLKSEL_CON39` `0x39c` 复位 `0x2CB`（p.73）；`CRU_GATE_CON11` `0x82c` 复位 `0x0`，bit8/9（p.120）；`CRU_SOFTRST_CON11` `0xa2c` 复位 `0x0`，bit8/9（p.172）；三者都有 bits31:16 write-enable。
  - Pinmux：`BUS_IOC_GPIO0B_IOMUX_SEL_H`（0x000C）`CAN0_TX_M0 = 0xb`；`BUS_IOC_GPIO0C_IOMUX_SEL_L`（0x0010）`CAN0_RX_M0 = 0xb`（寄存器细节 p.985；引脚复用表 p.1428）。
- Part2：`http://www.scs.stanford.edu/~zyedidia/docs/rockchip/rk3588_part2.pdf`（也含引脚复用表；本文的 CAN0 m0/m1 bank/pin 以 Part1 为准）。

**Bao**

- `bao:src/core/inc/vm.h:33-40` — `struct vm_dev_region`。
- `bao:src/core/vm.c:209-237` — `vm_init_dev()`，devs 映射 + IRQ 分配 + `io_vm_add_device`。
- `bao:src/core/mmu/mem.c:309,968-979` — `mem_alloc_vpage()` / `mem_alloc_map_dev()`。
- `bao:src/core/mem.c:319-445` — `mem_init_reserved()` 只预留 image/regions/shmem。
- `bao:src/core/interrupts.c:111-126` — `interrupts_vm_assign()` 独占。
- `bao:src/arch/armv8/interrupts.c:51-65` — SPI 冲突检查 + `vgic_set_hw`。
- `bao:src/arch/armv8/armv8-a/iommu.c:12-20,72-88` — SMMU 只在 `platform.arch.smmu.base` 时启用；`dev->id` 是 DMA stream ID。
- `bao:src/arch/armv8/inc/arch/gic.h:19` — Bao 的 GIC 最大中断数常量 = 1024。
- `bao:configs/example/config.c:69-79,139-143` — `devs[]` 语法与 interrupt-only dev。
- `bao:src/platform/`、`bao:configs/` — 无 rk3588 平台/配置（本 checkout）。

**Linux**

- `linux:drivers/clk/rockchip/clk-rk3588.c:522-524,1063-1077` — HIWORD 标志；CAN0/CAN1/CAN2 gate/mux/div。
- `linux:drivers/clk/rockchip/clk.h:26,492-493` — `HIWORD_UPDATE`；`RK3588_CLKSEL_CON`/`CLKGATE_CON`。
- `linux:drivers/clk/rockchip/rst-rk3588.c:14-19,127-132,853-857` — CRU reset 偏移 LUT；CAN0 reset 在 reg 11 bit8/9；HIWORD 标志在 853-857。
- `linux:drivers/clk/clk.c:1470-1509,1511-1526,1558` — `clk_disable_unused` / `clk_ignore_unused`。
- `linux:arch/arm64/boot/dts/rockchip/rk3588s-lubancat4-baovm.dts:22` — bootargs 含 `clk_ignore_unused`。
- `linux:arch/arm64/boot/dts/rockchip/rk3588s-lubancat4-baovm.dts:77-81,840-842` — PWM0 fan；`pwm0m0_pins` 与 CAN0_TX_M0 同脚。
- `linux:arch/arm64/boot/dts/rockchip/rk3588-base.dtsi:729-733,742-753,3538-3542` — `ioc@fd5f0000`、`cru@fd7c0000/0x5c000`、pinctrl `rockchip,grf = <&ioc>`。
- `linux:arch/arm64/boot/dts/rockchip/rk3588-base-pinctrl.dtsi:70-127,2205-2209,2650-2668` — can0m0/can1m0；pwm0m0；sdmmc_clk/cmd。
- `linux:drivers/pinctrl/pinctrl-rockchip.c:1227-1248,1356-1390,3014-3019` — bank0 双段 mux（PMU2 + BUS_IOC）；IOC 偏移常量。
- `linux:drivers/net/can/rockchip/rockchip_canfd.h:23-45,297` — MODE 位；erratum 5 的 300 MHz 阈值。
- `linux:drivers/net/can/rockchip/rockchip_canfd-core.c:266-277,795-800` — loopback 位；低于 300 MHz 的警告。

**Zephyr**

- `zephyr:dts/arm64/rockchip/rk3588.dtsi:81-90,118-134` — GIC 4-cell；CRU `0xfd7c0000/0x5c000`。
- `zephyr:dts/arm64/rockchip/rk3588s-baovm.dtsi:83-93` — `bus_ioc@fd5f8000/0x3000`、pinctrl `rockchip,grf = <&bus_ioc>`。
- `zephyr:boards/baovm/lubancat4_vm1/lubancat4_vm1.dts:84-88` — IRQ 编号规则：DT SPI + 32 = Bao INTID；333→365、422→454、420→452。
- `zephyr:scripts/dts/gen_defines.py:498-508` — GIC_SPI +32、GIC_PPI +16。
- `zephyr:drivers/clock_control/clock_control_rk3588_cru.c:30-72,74-86,121-149` — gate 查表、hiword 写、无 set_rate。
- `zephyr:drivers/reset/reset_rk3588_cru.c:15,25-37` — `0xa00 + (id/16)*4`、hiword reset。
- `zephyr:include/zephyr/dt-bindings/reset/rk3588-cru-reset.h:11-12` — GMAC 522/523（`reg*16+bit` 编码）。
- `zephyr:include/zephyr/dt-bindings/clock/rk3588-cru.h:11-19` — 当前只有 GMAC clock ID。
- `zephyr:include/zephyr/dt-bindings/pinctrl/rockchip-rk3588-pinctrl.h:11-51` — 当前只有 SPI0 M2 宏。
- `zephyr:docs/research/rk3588-native-can.md` — 驱动/寄存器/erratum 的上一轮研究（非 primary source，仅交叉引用）。
