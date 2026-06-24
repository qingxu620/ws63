# WS63 Screen 通信模式与最终链路判断

## 1. 目标与边界

本文档定义 Screen 在 Host Online Mode 与 Panel Offline Mode 下的最终通信链路、状态镜像原则、任务数据归属和 RX owner 模型。

当前阶段仅做文档设计，不实现真实 UART / SLE / SD / 音频 / 小游戏，不修改 TX/RX/Host 正式工程，不修改冻结目录。

## 2. 总体结论

最终一句话架构：

```text
TX 是电脑模式的无线入口；
Screen 是设备本体 HMI，并在离线模式下升级为无线任务入口；
RX 是唯一执行端和状态真相源。
```

三者职责：

| 节点 | Host Online Mode | Panel Offline Mode |
| --- | --- | --- |
| Host UI | 电脑端任务上传和操作入口 | 不参与 |
| TX | 电脑模式的无线入口，转发 Host job/control 到 RX | 可断电 / 不参与 |
| Screen | RX 状态监听器、本地安全面板、完成提醒器、诊断显示面板 | 取代 TX 生态位，成为离线 job owner 和无线任务入口 |
| RX | 唯一执行端、状态真相源、安全互锁源 | 唯一执行端、状态真相源、安全互锁源 |

### 2.1 能力状态与 fallback 路线

已采纳的最终主线不再依赖 `RX -> TX + Screen` 同时直连广播/组播。

前期验证结论：

- 普通 SLE SSAP 的 `ssaps_notify_indicate(conn_id=0xffff)` 语义是“发给所有已连接 peer”，不是无连接空中广播。
- 当前 RX 作为 SSAP Server 时，实测只能同时连接一个 Central。
- vendor `sle_one_to_many` 证明的是“一个 Client 同时连接多个 Server”，不是“一个 Server 同时被多个 Client 连接”。
- CHBA 支持多设备组网，但属于另一套更重的网络模型，不作为当前稳定激光链路主线。

因此 Host Online Mode 采纳 TX 转发状态给 Screen 的路线：

| 路线 | 结论 | 说明 |
| --- | --- | --- |
| Adopted | Host Online 主线 | TX 接收 RX 状态后转发给 Screen |
| Adopted | Panel Offline 主线 | Screen 单独连接 RX，取代 TX 成为离线 owner |
| Fallback | Host Online 最低可用 | Screen 只显示 fake/local diagnostics，不显示真实 RX 状态 |
| Research only | 长期实验 | CHBA 或 RX-as-Client 多连接状态镜像实验 |

该路线不改变最终职责判断：TX 是电脑模式无线入口，Screen 是设备本体 HMI 和离线任务入口，RX 是唯一执行端和状态真相源。

## 3. Host Online Mode 最终推荐链路

Host Online Mode 的任务数据主链路：

```text
Host UI -> TX -> RX
```

Host Online Mode 的状态镜像链路：

```text
RX -> TX -> Screen
```

TX 在 Host Online Mode 下同时承担两个职责：

- 作为 Host/PC 到 RX 的唯一无线任务入口。
- 作为 RX 状态到 Screen 的镜像转发器。

Screen 在 Host 模式下：

- 不上传 G-code。
- 不发送 JOB_DATA。
- 不抢 START。
- 不作为 job owner。
- 通过 TX 镜像读取 RX 状态。
- 作为本地安全面板。
- 作为完成提醒器。
- 作为诊断显示面板。

Screen 在 Host Online Mode 可通过 TX 请求的命令：

```text
STATUS
EXEC_STOP
ABORT
FOCUS_OFF
```

Screen 在 Host Online Mode 禁止请求的命令：

```text
BEGIN
DATA
END
EXEC_START
FOCUS_ON
```

原因：

- `BEGIN/DATA/END` 是任务上传数据流，必须属于当前 owner。
- `EXEC_START` 会改变任务执行状态，不应由非 owner 抢占。
- `FOCUS_ON` 会打开激光调焦输出，不应由非 owner 在 Host 任务上下文中触发。
- `STOP/ABORT/FOCUS_OFF` 属于安全类命令，允许非 owner 请求，但必须由 RX 最终确认。

Host Online Mode 下不推荐 Screen 直连 RX。若未来做研究性直连状态监听，权限仍必须收窄为：

允许：

```text
STATUS
EXEC_STOP
ABORT
FOCUS_OFF
```

禁止：

```text
BEGIN
DATA
END
EXEC_START
FOCUS_ON
```

Screen 直连 RX 不能被解释为第二个 Host，也不能消费 Host job 的可靠传输 ACK/NACK。当前产品主线不依赖该直连能力。

## 4. Panel Offline Mode 最终推荐链路

Panel Offline Mode 的最终任务链路：

```text
Screen -> SD read -> SLE Job Client -> RX
```

该模式下：

- TX 可以断电。
- Host 不参与。
- Screen 取代 TX 的生态位。
- Screen 成为离线模式下的 job owner。
- Screen 是离线模式下的无线任务入口。

Screen 在 Offline Mode 负责：

- SD 文件选择。
- G-code 读取。
- chunk 打包。
- ACK/NACK/retry/timeout。
- 执行监控。
- 完成提醒。
- STOP / ABORT / FOCUS_OFF 等安全操作。

Screen 在 Offline Mode 不应绕过 RX：

- 不直接执行运动。
- 不直接控制激光 GPIO/PWM。
- 不直接访问 DAC。
- 不绕过 RX 的 FOCUS_ON 安全互锁。

## 5. 状态镜像适合广播/转发，任务数据必须点对点

状态是只读信息，多个观察者同时接收不会改变执行结果。因此状态可以被 TX 转发给 Screen，也可以在长期实验中通过广播/组播承载。

当前采纳路线是 TX 转发，不要求 RX 同时连接 TX 与 Screen。

### 5.1 可镜像/广播/组播的内容

可镜像、广播或组播：

```text
PANEL_STATUS
job state
progress
executed lines
focus state
laser state
DONE event
ERROR event
```

其中 `DONE event` / `ERROR event` 必须携带 `seq` 或 `event_id`，避免 Screen 因丢包、乱序或重复接收而重复弹窗、重复提示音或错误覆盖。

用途：

- Screen 在 Host Online Mode 下实时显示。
- TX/Host 接收 RX 状态后转发给 Screen。
- Diagnostics 页面可显示当前 RX 真相状态。

### 5.2 不应广播的内容

不应广播：

```text
JOB_DATA
ACK/NACK offset 推进
BEGIN
DATA
END
EXEC_START
FOCUS_ON
owner claim
```

原因：

- `JOB_DATA` 是任务数据流，必须属于单一 owner，不能被多个端同时消费或推进。
- `BEGIN/DATA/END` 是可靠任务上传事务，必须是点对点 owner 链路。
- `owner claim` 是控制权变更，必须是有确认的点对点事务。
- `EXEC_START` 会改变执行状态，必须由当前 owner 发起并由 RX 确认。
- `FOCUS_ON` 会打开激光调焦输出，必须由当前 owner 或安全空闲条件下的新 owner 发起。
- ACK/NACK offset 推进是可靠传输机制，必须回到当前 owner，否则会造成重传、offset、seq、cache 状态混乱。

### 5.3 ACK/NACK 必须回给当前 owner

规则：

```text
owner=HOST   -> ACK/NACK 回 TX
owner=SCREEN -> ACK/NACK 回 Screen
```

严格边界：

- `owner=HOST` 时，ACK/NACK 只回 TX。
- `owner=SCREEN` 时，ACK/NACK 只回 Screen。
- Screen 不得消费 Host job 的 ACK/NACK。
- TX 不得消费 Screen job 的 ACK/NACK。

Host owner 时：

```text
Host UI -> TX -> RX
RX ACK/NACK -> TX -> Host UI
TX -> Screen 状态镜像
Screen 不参与 ACK/NACK 推进
```

Screen owner 时：

```text
Screen -> RX
RX ACK/NACK -> Screen
TX/Host 不参与 ACK/NACK 推进
```

## 6. RX owner 模型

RX 侧最终应维护或确认控制权：

```text
control_owner = NONE / HOST / SCREEN
```

该 owner 是 RX 执行与任务数据归属的最终判据。

### 6.1 owner 进入规则

Host 经 TX 上传或执行时：

```text
owner = HOST
```

Screen 离线发送或执行时：

```text
owner = SCREEN
```

无任务、无上传、无执行、无调焦请求时：

```text
owner = NONE
```

### 6.2 owner 权限规则

只能由当前 owner 或 owner 为 `NONE` 时的新 owner 发起：

```text
START
SEND
FOCUS_ON
BEGIN
DATA
END
EXEC_START
```

安全类命令允许非 owner 请求：

```text
STOP
ABORT
FOCUS_OFF
```

但必须满足：

- 由 RX 最终确认。
- 由 RX 执行安全动作。
- Screen/Host 不得本地假设成功。

### 6.3 owner 拒绝示例

Host 正在上传：

```text
owner=HOST
Screen BEGIN/DATA/EXEC_START/FOCUS_ON -> reject
Screen STATUS/EXEC_STOP/ABORT/FOCUS_OFF -> allow request
```

Screen 离线任务正在执行：

```text
owner=SCREEN
Host BEGIN/DATA/EXEC_START/FOCUS_ON -> reject
Host STATUS/EXEC_STOP/ABORT/FOCUS_OFF -> allow request
```

## 7. PANEL_STATUS 字段建议

TX 镜像给 Screen 的 `PANEL_STATUS` 至少应包含：

```text
seq
owner
mode
job_state
job_id
received_size
total_size
executed_lines
cache_free
focus_active
laser_output_active
last_error
tick/timestamp
```

字段含义：

| 字段 | 含义 |
| --- | --- |
| `seq` | 状态序号，用于 Screen 判断丢包或乱序 |
| `owner` | `NONE` / `HOST` / `SCREEN` |
| `mode` | `IDLE` / `ONLINE` / `OFFLINE` / `ERROR` 等 |
| `job_state` | RX 当前任务状态 |
| `job_id` | 当前任务 ID |
| `received_size` | RX 已接收字节数 |
| `total_size` | 当前任务总字节数 |
| `executed_lines` | 已执行行数 |
| `cache_free` | RX cache 剩余空间 |
| `focus_active` | RX 确认的调焦状态 |
| `laser_output_active` | RX 确认的激光输出状态 |
| `last_error` | 最近错误码或错误原因 |
| `tick/timestamp` | RX 时间戳或系统 tick |

建议文本形态：

```text
@PANEL_STATUS seq=42 owner=HOST mode=ONLINE job_state=EXECUTING job=7 rx=4096 total=8192 lines=123 free=120000 focus=0 laser=1 err=0 tick=123456
```

约束：

- `PANEL_STATUS` 是只读状态，不推进任务。
- Screen 只能用它刷新 UI 和诊断。
- 不能把 `PANEL_STATUS` 当作 ACK/NACK。

## 8. Screen 在 Host Online Mode 的状态策略

Screen 在 Host Online Mode 下应遵守：

- `owner=HOST` 时，Screen 禁用 START。
- `owner=HOST` 时，Screen 禁用 SEND。
- `owner=HOST` 时，Screen 禁用 FOCUS_ON。
- Screen 可显示 STOP / ABORT / FOCUS_OFF。
- Screen 点击 STOP / ABORT / FOCUS_OFF 后进入 `REQUESTING_*`。
- Screen 必须等待 RX 状态或响应确认后更新 UI。
- RX 状态优先于 Screen 本地状态。

推荐 UI 文案：

```text
控制源：HOST
模式：ONLINE
本机屏幕：监控 / 安全面板
```

## 9. Screen 在 Panel Offline Mode 的状态策略

Screen 在 Panel Offline Mode 下应遵守：

- Screen 可 claim owner。
- Screen 是 job owner。
- Screen 负责 SD -> chunk -> RX 的可靠发送。
- ACK/NACK/retry/timeout 只在 Screen 与 RX 之间推进。
- TX/Host 不参与离线任务数据流。
- RX 仍是执行和安全真相源。

推荐 UI 文案：

```text
控制源：SCREEN
模式：OFFLINE
离线任务：发送 / 执行 / 完成
```

## 10. 与现有设计文档的关系

- `screen_role_and_feature_scope.md` 定义 Screen 的产品角色和功能层级。
- `screen_tx_integration_plan.md` 定义 Host Online Mode 下经 TX 接入和状态镜像的主线。
- 本文档修正最终通信判断：Host Online Mode 使用 `RX -> TX -> Screen` 状态镜像让 Screen 成为实时 HMI；Panel Offline Mode 中 Screen 取代 TX 的生态位成为无线任务入口。

短期仍不实现真实通信。当前 UI 原型应继续使用 fake model 验证信息架构。

## 11. untracked 文档审核命令

当前文档可能处于 untracked 状态。审核时不要只依赖 `git diff --stat`，应同时检查 untracked 文档：

```bash
git status --short
git diff --stat
git ls-files --others --exclude-standard src/ws63_screen_panel_lvgl_refactor/docs/
```

说明：

- `git diff --stat` 只统计已跟踪文件的工作区差异。
- 新增但未 `git add` 的文档需要通过 `git status --short` 或 `git ls-files --others --exclude-standard` 查看。
- 当前任务禁止 `git add` 和 commit。
