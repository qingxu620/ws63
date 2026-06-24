# WS63 Screen 早期 TX 接入设计

## 1. 目标与边界

本文档定义 `src/ws63_screen_panel_lvgl_refactor` 早期接入当前稳定 Host/TX/RX 链路时的保守路线、fallback 路线、阶段拆分、协议最小集和模块职责边界。

本文件不是最终通信形态定义，不应再理解为“Screen 永远必须接 TX”。

最终推荐架构以 `communication_modes.md` 和 `screen_role_and_feature_scope.md` 中的“RX 状态广播 + Screen 离线 Job Client”为准：

```text
Host Online Mode：Host UI -> TX -> RX；RX -> TX + Screen 状态广播/组播
Panel Offline Mode：Screen -> SD read -> SLE Job Client -> RX
```

也就是说，本文档描述的是“先通过 TX 做最小闭环”的早期路线，不是最终形态。最终形态中：

- TX 是电脑模式的无线入口。
- Screen 是设备本体 HMI，并在离线模式下升级为无线任务入口。
- RX 是唯一执行端和状态真相源。

当前阶段只做设计准备，不实现真实链路。

注意：`RX -> TX + Screen 状态广播/组播` 是最终推荐目标，不是当前已验证能力。是否可行取决于 WS63 SDK 对多连接、广播业务、组播业务或多接收端 notification 的支持。当前阶段不得假设 SDK 已经支持。

硬性边界：

- 不修改 `src/ws63_screen_panel_lvgl` 冻结目录。
- 不修改 TX 正式工程 `src/ws63_laser_sle_job`。
- 不修改 RX 正式工程 `src/ws63_laser_rx_unified`。
- 不修改 Host 正式工程 `src/ws63_laser_host_ui`。
- 不接真实 UART / SLE / SD / 音频 / 小游戏。
- 不 `git add`，不 commit。

## 2. 早期保守架构

早期保守架构是：Host UI 和 Screen UI 都接入 TX，由 TX 做轻量仲裁，TX 再通过现有 SLE Job Packet 接 RX。

该方案适合 Phase B1/B2 这类最小闭环验证，原因是它复用当前稳定 TX/RX 链路，不要求 RX 立即支持 Screen 直连 observer、状态广播/组播或离线 owner。

它也对应最终架构无法马上验证时的 fallback：

| 路线 | 说明 |
| --- | --- |
| Preferred | RX 状态广播/组播给 TX + Screen |
| Fallback A | Host Online 阶段由 TX 转发 RX 状态给 Screen |
| Fallback B | Host Online 阶段 Screen 只显示 fake/local diagnostics |
| Fallback C | 优先实现 Panel Offline 单独连接 RX，Host Online 状态镜像后置 |

```text
Host UI
src/ws63_laser_host_ui
    │
    │ 文本控制命令
    ▼
TX control_dispatcher
src/ws63_laser_sle_job/transmitter
    ▲
    │ 文本控制命令
    │
Screen UI
src/ws63_screen_panel_lvgl_refactor

TX
    │ SLE Job Packet
    ▼
RX
src/ws63_laser_rx_unified
    │
    ▼
Motion / DAC / Laser PWM / safety interlock
```

核心原则：

- RX 仍然是运动执行、任务状态、激光状态、安全互锁的源头。
- TX 在早期路线中是 Host 与 Screen 的统一入口仲裁点。
- Screen 不直接证明真实激光状态，只显示 TX/RX 回包确认后的状态。
- Screen 不上传 G-code，不解析 G-code，不生成运动轨迹。
- 最终 Host Online Mode 应演进为 RX 状态广播/组播给 TX + Screen，Screen 作为状态监听器和本地安全面板。
- 最终 Panel Offline Mode 中 Screen 取代 TX 的生态位，成为离线 job owner 和无线任务入口。
- 在 SDK 多接收端能力未验证前，本文档的 TX 转发路线只应作为早期保守路线或 fallback，不应固化为最终产品架构。

## 3. 明确否定的方案

### 3.1 否定 Host Online Mode 下 Screen 作为第二主控直连 RX

不推荐：

```text
Host -> TX -> RX
Screen -----> RX  （同时作为第二主控发送任务/START/FOCUS_ON）
```

原因：

- 会形成两个独立控制入口。
- Host 和 Screen 可能同时下发 `START`、`STOP`、`ABORT`、`FOCUS_ON`。
- 仲裁职责会下沉到 RX，增加执行端复杂度。
- 容易破坏当前稳定 SLE Job Packet 链路。
- 不符合“不要创建两个独立 Grbl/Job 控制器”的工程约束。

注意：这不否定 `communication_modes.md` 定义的最终方案：

- Host Online Mode 下，Screen 可以作为 RX 状态监听器接收状态广播/组播。
- Panel Offline Mode 下，Screen 可以取代 TX 的生态位，作为 owner 与 RX 建立离线任务链路。
- 关键区别是 owner：任务数据、`EXEC_START`、`FOCUS_ON`、ACK/NACK 推进逻辑必须属于单一 owner。

### 3.2 否定 LVGL 并入 RX

不推荐：

```text
RX = SLE + parser + motion + laser + LVGL + touch + LCD SPI
```

原因：

- RX 承担运动、缓存、解析、激光安全和实时输出，优先级应服务执行稳定性。
- LVGL 刷屏和触摸处理会增加调度压力。
- LCD SPI 刷新和背光 PWM 调试仍需独立收敛，不应进入 RX 实时路径。
- 一旦 UI 卡顿或显示异常，不应影响 RX 激光安全逻辑。

## 4. 分阶段路线

### Phase A：UI 原型，不接 TX/RX

目标：

- `src/ws63_screen_panel_lvgl_refactor` 保持独立屏幕固件。
- UI 使用本地 fake/demo model。
- 不连接真实 UART/SLE/RX/Host。
- 优先修稳显示、触摸、亮度、布局、中文字体和交互反馈。

验收：

- 上电显示稳定。
- 触摸响应稳定。
- 页面切换和滚动稳定。
- 设置页亮度调节不破坏 LCD 图像。
- `./scripts/build_screen_firmware.sh --panel` 通过。

### Phase B1：Screen -> TX `@STATUS` 最小闭环

目标：

- Screen 只向 TX 请求状态。
- TX 返回 RX 状态文本。
- Screen 只读显示，不发送控制动作。

允许命令：

```text
@STATUS
```

验收：

- Screen 能显示 TX/RX 连接状态。
- Screen 能显示 RX job state、job id、接收进度、执行行数。
- Host 原有流程不受影响。

### Phase B2：STOP / ABORT / FOCUS_OFF

目标：

- 先接入低风险安全控制。
- 不接 `START`，不接 `FOCUS_ON`。

允许命令：

```text
@EXEC_STOP
@ABORT
@FOCUS_OFF
```

原则：

- `FOCUS_OFF` 应无条件允许。
- `STOP` / `ABORT` 属于安全优先命令。
- Screen 发送命令后进入 `REQUESTING`，收到 TX/RX 确认后更新 UI。

### Phase B3：FOCUS_ON/OFF（需要 owner 约束）

目标：

- 接入调焦光开关。
- `FOCUS_ON` 必须由 RX 安全互锁最终确认。
- Host Online Mode 且 `owner=HOST` 时，Screen 禁止请求 `FOCUS_ON`。
- `FOCUS_ON` 更适合在 owner 为 `SCREEN` 的离线模式，或 owner 为 `NONE` 且系统安全空闲时开放。

允许命令：

```text
@FOCUS_ON S10
@FOCUS_OFF
```

原则：

- `S10` 是推荐默认低功率调焦值。
- Screen 点击调焦开后不能直接显示红色已开启。
- Screen 必须先显示 `REQUESTING`。
- 收到 `@OK focus_on ...` 或后续状态 `focus=1` 后才显示已开启。
- 收到 `@OK focus_off` 或状态 `focus=0` 后才显示已关闭。
- 任何 `EXEC_START`、`ABORT`、disconnect、error 都应强制 UI 显示 focus off。

### Phase B4：最后接 START（需要 owner 约束）

目标：

- 在状态显示、停止、中止、调焦都稳定后，再接入启动。

后续命令：

```text
@EXEC_START <job_id>
```

接入条件：

- TX 能判断当前是否存在已缓存 job。
- RX 状态为 `JOB_READY`。
- Host 没有正在上传/执行/控制的事务。
- Screen 当前不处于其它控制请求中。
- RX owner 为 `NONE` 或 `SCREEN`，不得抢占 `owner=HOST` 的任务上下文。

原则：

- Screen 点击 START 后只进入 `REQUESTING_START`。
- 只有收到 TX/RX 确认后才能显示 running/executing。
- 若 TX 返回 busy 或 RX 拒绝，Screen 必须回到可解释状态并显示错误。

## 5. Screen-TX 文本协议最小集

Phase B 文本命令池：

```text
@STATUS
@EXEC_STOP
@ABORT
@FOCUS_ON S10
@FOCUS_OFF
```

其中 Host Online Mode 且 `owner=HOST` 时，Screen 实际允许命令应收窄为：

```text
@STATUS
@EXEC_STOP
@ABORT
@FOCUS_OFF
```

`@FOCUS_ON S10` 只能在 owner 规则允许时开放，例如 `owner=SCREEN` 的离线模式，或 `owner=NONE` 且 RX 确认安全空闲。

后续才考虑：

```text
@EXEC_START <job_id>
```

暂不纳入 Screen：

- `@BEGIN`
- `@DATA`
- `@END`
- G-code 上传
- G-code 解析
- route switch
- WiFi 配置
- SD 文件读取

## 6. TX 返回格式

TX 给 Screen 的返回分为五类。

### 6.1 标准 RX 状态

兼容当前 Host 已使用格式：

```text
@STATUS state=<n> status=<n> job=<id> rx=<bytes> total=<bytes> free=<bytes> lines=<n>
```

用途：

- 基础 job state。
- 接收进度。
- 缓存剩余。
- 已执行行数。

### 6.2 面板扩展状态

建议新增只读扩展行：

```text
@PANEL_STATUS mode=<mode> route=<route> tx=<0|1> rx=<0|1> focus=<0|1> laser=<0|1> busy=<0|1> state=<text>
```

示例：

```text
@PANEL_STATUS mode=SLE_JOB route=SLE_JOB tx=1 rx=1 focus=0 laser=0 busy=0 state=IDLE
```

用途：

- 不破坏现有 Host `@STATUS` 解析。
- 给 Screen 提供更直接的显示字段。
- 后续 Host 也可选择解析。

### 6.3 成功确认

```text
@OK <command> [fields...]
```

示例：

```text
@OK focus_off
@OK focus_on s=10
@OK exec_stop
```

### 6.4 拒绝确认

```text
@NACK <reason> [fields...]
```

示例：

```text
@NACK focus_reject
@NACK busy owner=host
@NACK bad_state state=EXECUTING
```

### 6.5 错误

```text
@ERR <reason> [fields...]
```

示例：

```text
@ERR tx_not_connected
@ERR rx_timeout
@ERR command_not_allowed
@ERR malformed_command
```

## 7. 屏幕状态原则

Screen UI 必须遵守以下规则：

1. UI 不能因为本地按钮点击直接认为 `RUNNING`。
2. UI 不能因为本地按钮点击直接认为 `FOCUS_ON`。
3. 本地按钮点击后进入 `REQUESTING_*` 中间态。
4. 只有收到 TX/RX 回包后才能确认状态。
5. RX 状态优先级高于本地 UI 状态。
6. TX/RX timeout 时，UI 必须显示未知或失败，不得假设成功。
7. 安全相关状态必须保守显示：
   - link lost -> focus off / laser unknown-safe
   - abort -> focus off
   - exec start -> focus off
   - error -> focus off

建议 Screen 内部状态机：

```text
IDLE
RECEIVING
JOB_READY
REQUESTING_START
EXECUTING
REQUESTING_STOP
REQUESTING_ABORT
REQUESTING_FOCUS_ON
REQUESTING_FOCUS_OFF
ERROR
LINK_LOST
```

## 8. TX control_dispatcher 职责边界

未来 TX 侧建议抽出 `control_dispatcher`，作为 Host 和 Screen 的共同控制入口。

### 8.1 应负责

- 入口仲裁：
  - command source = Host / Screen
  - 当前是否已有事务在进行
- 命令白名单：
  - Screen 初期只允许 `STATUS`、`STOP`、`ABORT`、`FOCUS_OFF`
  - 后续逐步开放 `FOCUS_ON` 和 `START`
- busy 判断：
  - Host 上传中，Screen 不能启动 job。
  - Screen 请求中，Host 不应并发下发冲突控制。
  - `STATUS` 可并发或低优先级排队。
- 调用现有 SLE 发送函数：
  - 不重写 SLE packet 编码。
  - 不改变 ACK/NACK 机制。
- 响应格式化：
  - `@STATUS ...`
  - `@PANEL_STATUS ...`
  - `@OK ...`
  - `@NACK ...`
  - `@ERR ...`

### 8.2 不应负责

- 不解析 G-code。
- 不生成运动行为。
- 不控制激光 GPIO。
- 不控制激光 PWM。
- 不直接访问 DAC。
- 不绕过 RX 安全互锁。
- 不替代 RX job manager。
- 不创建第二套 Grbl/Job 控制器。

## 9. 未来文件建议

### 9.1 Screen 侧建议文件

位于：

```text
src/ws63_screen_panel_lvgl_refactor/src/service/
```

建议新增：

```text
panel_transport.h
panel_transport_uart.c
panel_protocol.h
panel_protocol.c
panel_state.h
panel_state.c
```

职责划分：

- `panel_transport.h`
  - 定义 Screen-TX transport 抽象。
  - 当前可先提供 fake transport。
- `panel_transport_uart.c`
  - 未来真实 UART 实现。
  - 当前阶段不接。
- `panel_protocol.h`
  - 定义可解析的文本返回类型。
  - 定义命令构造函数声明。
- `panel_protocol.c`
  - 解析 `@STATUS`、`@PANEL_STATUS`、`@OK`、`@NACK`、`@ERR`。
  - 不直接更新 UI。
- `panel_state.h`
  - 定义 Screen UI 状态模型。
- `panel_state.c`
  - 把协议事件归并成 UI 可消费状态。
  - 处理 `REQUESTING`、timeout、link lost。

### 9.2 TX 侧建议文件

位于：

```text
src/ws63_laser_sle_job/transmitter/
```

建议新增：

```text
control_dispatcher.c
control_dispatcher.h
screen_uart.c
screen_uart.h
```

职责划分：

- `control_dispatcher.c/.h`
  - Host/Screen 统一入口仲裁。
  - 命令白名单。
  - busy 判断。
  - 调用现有 SLE 发送函数。
  - 格式化返回。
- `screen_uart.c/.h`
  - 未来 Screen 专用 UART 输入。
  - 不复制 Host 的完整上传路径。
  - 只转发白名单控制命令。

### 9.3 RX 侧建议扩展

位于：

```text
src/ws63_laser_rx_unified/routes/sle_job/
```

建议：

- 保持现有 SLE Job Packet 主流程不变。
- 增加 `@PANEL_STATUS` 所需只读状态扩展。
- 不改变 job data framing。
- 不改变 ACK/NACK 语义。
- 不改变 cache/preroll 逻辑。

建议状态字段：

```text
mode
active_route
job_state
job_id
received_size
total_size
cache_free
executed_lines
focus_active
laser_output_active
busy
safe_stop_reason
```

## 10. 验收路线

### Phase B1 验收

- Screen 能发送 `@STATUS`。
- TX 能返回 `@STATUS ...`。
- Screen 能解析并显示基础状态。
- Host 原有 `@STATUS` 不受影响。

### Phase B2 验收

- Screen 发送 `@FOCUS_OFF`，任意状态下 RX 最终关闭 focus。
- Screen 发送 `@EXEC_STOP`，执行中能安全停止。
- Screen 发送 `@ABORT`，job/cache 状态清理符合现有 RX 逻辑。
- Host 上传中 Screen 不允许发冲突 START。

### Phase B3 验收

- Screen `FOCUS_ON S10` 只在 RX idle 且安全条件满足时成功。
- Screen 红色 focus 状态只来自 TX/RX 回包。
- `EXEC_START`、`ABORT`、disconnect、error 后 Screen 强制显示 focus off。

### Phase B4 验收

- 只有 `JOB_READY` 时 Screen START 可用。
- Screen START 后先显示 `REQUESTING_START`。
- RX ACK/状态确认后显示 executing。
- busy / bad state / timeout 都能显示明确失败。

## 11. 当前阶段结论

当前最稳妥的早期路线是：

1. `src/ws63_screen_panel_lvgl_refactor` 继续先做独立 UI 原型。
2. 早期接入时先让 Screen 通过 TX 查询 `@STATUS`。
3. Host Online Mode 下优先开放状态显示、`FOCUS_OFF`、`STOP`、`ABORT`。
4. `FOCUS_ON` 和 `START` 必须等待 owner 规则明确后再开放。
5. 最终通信判断迁移到 `communication_modes.md`：Host 模式用 RX 状态广播/组播支撑 Screen HMI；离线模式由 Screen 取代 TX 生态位成为 job owner。
6. 不把 LVGL 并入 RX。
