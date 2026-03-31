# Rockchip CAN Driver Test Suite

完整的测试框架为 Rockchip CAN 驱动程序提供了全面的测试覆盖。

## 测试模块概述

### 1. 初始化测试 (`init_test.c`)
- **测试设备就绪状态**
- **获取驱动能力**
- **检查最大过滤器数量**
- **验证初始设备状态**
- **设置模式**
- **获取核心时钟频率**

### 2. 模式与时序测试 (`mode_timing_test.c`)
- **CAN 模式设置** (NORMAL, LOOPBACK, LISTENONLY, ONE_SHOT)
- **时序参数配置** (SJW, prescaler, phase_seg1, phase_seg2)
- **边界值测试** (最小/最大参数)
- **无效参数检测**
- **启动后的模式/时序修改限制**

### 3. 发送/接收测试 (`tx_rx_test.c`)
- **基本帧发送**
- **标准帧、扩展帧、RTR帧发送**
- **发送回调处理**
- **多帧连续发送**
- **启动/停止设备**
- **环回模式测试**
- **发送时的错误处理**

### 4. 过滤器测试 (`filtering_test.c`)
- **添加单个 RX 过滤器**
- **添加多个过滤器**
- **过滤器删除**
- **标准帧过滤**
- **扩展帧过滤**
- **带掩码的过滤器**
- **过滤器表填满**
- **过滤超容错误处理**
- **过滤器用户数据**

### 5. 中断和状态测试 (`interrupt_state_test.c`)
- **获取设备状态**
- **错误计数器**
- **状态变化回调**
- **各种模式下的状态**
- **手动恢复模式**
- **状态转换序列**

## 构建和运行

### 前置条件
- Zephyr SDK 已安装
- 支持 Rockchip CAN 的硬件或模拟器
- 设备树中配置了 CAN 节点

### 构建测试
```bash
cd zephyr
west build -b <board> -s tests/drivers/can/rockchip -d build/rockchip_can_test
```

### 运行测试
```bash
west build -d build/rockchip_can_test -t run
```

### 针对特定测试板的构建

对于 Rockchip 平台（如 RK3588）：
```bash
west build -b rk3588_evb -s tests/drivers/can/rockchip
```

## 测试覆盖率

| 功能 | 测试数量 | 覆盖范围 |
|------|--------|--------|
| 初始化 | 6 | 设备就绪、能力、时钟 |
| 模式配置 | 13 | 所有模式、边界值、错误处理 |
| TX/RX | 9 | 基本发送、各帧类型、环回 |
| 过滤 | 13 | 单/多过滤器、标准/扩展帧 |
| 状态/中断 | 13 | 状态变化、恢复模式、回调 |
| **总计** | **54+** | **完整功能验证** |

## 配置选项

### prj.conf - 主要配置

```kconfig
# CAN 驱动核心
CONFIG_CAN=y
CONFIG_CAN_ROCKCHIP=y

# 过滤器配置
CONFIG_CAN_ROCKCHIP_MAX_FILTERS=8

# 可选功能
CONFIG_CAN_MANUAL_RECOVERY_MODE=y
CONFIG_CAN_FD_MODE=y

# 日志级别
CONFIG_CAN_LOG_LEVEL_DBG=y
```

## 测试框架工具

### 信号量同步
- `rx_callback_sem` - RX 回调同步
- `tx_callback_sem` - TX 回调同步

### 全局测试数据
```c
extern const struct device *can_dev;           /* CAN 设备 */
extern struct can_frame last_rx_frame;         /* 最后接收的帧 */
extern int last_rx_filter_id;                 /* 最后使用的过滤器 ID */
```

### 测试帧定义
```c
test_std_frame_1          /* 标准帧 1 */
test_std_frame_2          /* 标准帧 2 */
test_ext_frame_1          /* 扩展帧 1 */
test_ext_frame_2          /* 扩展帧 2 */
test_std_rtr_frame_1      /* 标准 RTR 帧 */
test_ext_rtr_frame_1      /* 扩展 RTR 帧 */
```

## 运行单个测试

使用 `ztest` 过滤器运行特定的测试套件：

```bash
west build -d build/rockchip_can_test -t run -- --filter=rockchip_can_init
west build -d build/rockchip_can_test -t run -- --filter=rockchip_can_mode_timing
west build -d build/rockchip_can_test -t run -- --filter=rockchip_can_tx_rx
west build -d build/rockchip_can_test -t run -- --filter=rockchip_can_filtering
west build -d build/rockchip_can_test -t run -- --filter=rockchip_can_interrupt_state
```

## 调试故障排查

### 启用更详细的日志
```kconfig
CONFIG_LOG_MODE_IMMEDIATE=y
CONFIG_CAN_LOG_LEVEL_DBG=y
CONFIG_LOG_LEVEL=4
```

### 常见问题

1. **设备不就绪**
   - 检查设备树中 CAN 节点的配置
   - 确保 `zephyr_canbus` 被正确选择

2. **过滤器测试失败**
   - 确保 `CONFIG_CAN_ROCKCHIP_MAX_FILTERS` 满足测试需求
   - 检查过滤器数量配置

3. **状态转换问题**
   - 检查时序配置是否有效
   - 验证驱动程序是否收到中断

## 扩展测试

可以添加以下测试：
- CAN FD 模式测试
- 性能/压力测试（高频帧）
- 错误恢复场景
- 电源管理测试
- 并发 TX/RX 操作

## 相关文件

- `common.h/c` - 公共定义和实用函数
- `init_test.c` - 初始化和能力测试
- `mode_timing_test.c` - 模式和时序配置
- `tx_rx_test.c` - 发送/接收功能
- `filtering_test.c` - RX 过滤器
- `interrupt_state_test.c` - 状态和中断处理
