# WS63 Screen 角色定位与功能范围

## 1. 背景问题

如果 Screen 只被定义为“离线模式下的 SD + SLE Job Client”，那么在 Host 上位机控制设备时，屏幕会变成摆设。

这不符合设备本体 HMI 的价值。屏幕不应该只在离线模式下工作，而应该在任何设备开机运行场景中都有明确作用。

最终定位应修正为：

```text
Screen = Industrial HMI + Offline Job Controller
```

即：

- Host Online Mode：Screen 不是主控，但负责状态监控、本地安全操作、完成提醒、诊断和产品化展示。
- Panel Offline Mode：Screen 是离线任务主控，负责 SD 文件选择、任务下发和执行监控。

## 2. 总体角色定义

| 模式 | 控制主机 | Screen 作用 |
| --- | --- | --- |
| Host Online Mode | Host + TX | 状态监控屏、本地安全面板、完成提醒器、诊断面板、产品化 UI 展示窗口 |
| Panel Offline Mode | Screen | SD 文件浏览、文件详情、离线任务发送、执行监控、本地安全操作 |
| Idle Mode | 无明确 owner | 设备状态、连接状态、设置、诊断、待机展示、隐藏彩蛋入口 |

一句话定义：

```text
Screen 是激光打标机本体的工业 HMI：
在 Host 联机模式下负责监控、安全、提醒和展示；
在离线模式下负责 SD 文件选择、SLE 下发和执行监控。
```

最终一句话架构：

```text
TX 是电脑模式的无线入口；
Screen 是设备本体 HMI，并在离线模式下升级为无线任务入口；
RX 是唯一执行端和状态真相源。
```

注意：Host Online Mode 下不再采用 `RX -> TX + Screen` 同时直连广播/组播作为产品主线。前期验证表明普通 SLE SSAP Server 当前只能同时连接一个 Central，`conn_id=0xffff` 只能发给已连接 peer，不能解决第二个 observer 连接问题。

已采纳的通信主线：

| 模式 | 链路 | 定位 |
| --- | --- |
| Host Online | `Host / PC -> TX -> RX`，同时 `TX -> Screen` 状态镜像 | TX 是电脑模式无线入口，Screen 是 HMI |
| Panel Offline | `Screen -> RX` | Screen 取代 TX，成为离线 job owner |

CHBA 或 RX-as-Client 多连接状态镜像只作为长期研究项，不进入当前稳定主线。

## 3. Host Online Mode：Host 控制时 Screen 的作用

Host Online Mode 的任务数据主链路仍然是：

```text
Host UI -> TX -> RX
```

Host Online Mode 的状态镜像链路最终采用：

```text
RX -> TX -> Screen
```

TX 在 Host Online Mode 下接收 RX 的 `STATUS` / `PANEL_STATUS` / `OK` / `NACK` / `ERR` 等结果，并把 Screen 需要的只读状态镜像给 Screen。Screen 不直接连接 RX，不参与 Host job 的 ACK/NACK 推进。

Screen 在该模式下不是任务主控，不上传 G-code，不抢 START，但必须作为设备本体 HMI 持续提供价值。

该模式下 Screen 的角色是：

```text
RX 状态镜像显示器 + 本地安全面板 + 完成提醒器 + 诊断显示面板
```

TX 转发链路适合承载只读状态，例如 `PANEL_STATUS`、job state、progress、executed lines、focus state、laser state、DONE/ERROR event。DONE/ERROR event 必须带 `seq` 或 `event_id`，避免重复提示或乱序覆盖。

状态镜像链路不应用来承载 `JOB_DATA`、ACK/NACK offset 推进、`BEGIN/DATA/END`、`EXEC_START`、`FOCUS_ON` 或 owner claim。

Host Online 下，Screen 不推荐直连 RX。如果未来做研究性直连监听，它的权限仍必须限制为：

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

ACK/NACK 归属必须单一：`owner=HOST` 时只回 TX，Screen 不得消费 Host job 的 ACK/NACK。

### 3.1 状态监控屏

Screen 应显示：

```text
当前控制源：HOST
当前模式：ONLINE
RX job state：IDLE / RECEIVING / JOB_READY / EXECUTING / DONE / ERROR
任务名 / job id
接收进度
执行进度
executed lines
cache free
SLE link 状态
TX/RX link 状态
last error
laser state
focus state
```

目标：

- 电脑负责上传和复杂操作。
- 设备本体屏幕实时展示机器状态。
- 调试和展示时，不需要只依赖串口日志或 Host UI。

### 3.2 本地安全面板

Host 控制时，Screen 仍应提供现场安全类操作：

```text
STOP
ABORT
FOCUS_OFF
```

按钮权限建议：

| Host 控制状态 | Screen START | Screen STOP | Screen ABORT | Screen FOCUS_OFF |
| --- | ---: | ---: | ---: | ---: |
| 上传中 | 禁用 | 可用 | 可用 | 可用 |
| 执行中 | 禁用 | 可用 | 可用 | 可用 |
| 错误中 | 禁用 | 禁用/可选 | 可用 | 可用 |
| 空闲中 | 根据 owner/mode 决定 | 禁用 | 可用 | 可用 |

原则：

- 谁启动任务不重要，现场必须能停。
- Screen 的 STOP / ABORT / FOCUS_OFF 是安全类入口。
- 安全动作仍必须由 RX 最终确认和执行。

### 3.3 完成提醒器

Host 控制完成后，Screen 应提供设备本体反馈：

```text
DONE 页面
短提示音
任务耗时
执行行数
是否有错误
取件/检查提示
```

提示音不是音乐播放器功能，而是设备完成提醒器。

### 3.4 诊断面板

Host 控制时，Screen 还应提供 Diagnostics 页面：

```text
Host connected
TX connected
RX connected
SLE link
RSSI / link quality（未来可选）
RX cache free
RX last error
laser state
focus state
job state
firmware version
heap / fps（Screen 本地）
```

目标：

- 降低调试依赖。
- 现场快速判断 TX/RX/SLE/Screen 哪一层异常。

### 3.5 产品化 UI 展示窗口

Host 负责复杂任务上传时，Screen 可以专注展示成熟产品感：

```text
工业仪表盘
进度动画
状态卡片切换
错误 toast
完成弹窗
按钮反馈
页面切换
```

注意：

- LVGL UI 更新必须集中在 UI task。
- SLE / SD / 音频 / 传输任务未来应通过消息队列或状态缓存通知 UI，不得多任务并发调用 `lv_*`。

## 4. Panel Offline Mode：Screen 离线控制时的作用

Panel Offline Mode 的长期目标链路是：

```text
Screen -> SD read -> SLE Job Client -> RX
```

该模式下：

- Screen 是任务 owner。
- TX 可以不供电。
- Host 不参与。
- Screen 取代 TX 的生态位，成为离线模式下的无线任务入口。
- Screen 负责选择文件、发送 job、监控执行和安全控制。
- ACK/NACK/retry/timeout 必须在 Screen 与 RX 之间推进，TX/Host 不参与离线任务数据流。
- `owner=SCREEN` 时 ACK/NACK 只回 Screen，TX 不得消费 Screen job 的 ACK/NACK。

Screen 应提供：

```text
当前控制源：SCREEN
当前模式：OFFLINE
SD 文件浏览
文件详情
发送进度
RX 接收进度
执行监控
STOP
ABORT
FOCUS_OFF
完成提醒
错误显示
```

离线模式仍然不应绕过 RX：

- RX 仍是运动执行源。
- RX 仍是激光安全源。
- RX 仍负责 FOCUS_ON 安全互锁。

## 5. 控制权模型

引入文档级设计概念：

```text
control_owner = NONE / HOST / SCREEN
```

当前阶段不实现代码，只作为后续协议和 UI 状态设计依据。

最终实现时，RX 侧应维护或确认该 owner。Screen 和 TX/Host 的 UI 状态只能显示 owner，不应各自独立判定任务归属。

### 5.1 owner 规则

- Host 上传或执行时：

```text
owner = HOST
```

- Screen 离线任务发送或执行时：

```text
owner = SCREEN
```

- 空闲且没有任务事务时：

```text
owner = NONE
```

### 5.2 命令权限规则

普通任务类命令：

```text
START
SEND
FOCUS_ON
BEGIN
DATA
END
EXEC_START
```

规则：

- 只能由当前 owner 发起。
- owner 为 `NONE` 且系统安全空闲时，新请求方可 claim owner。
- Host owner 期间，Screen 不允许 START / SEND / FOCUS_ON。
- Screen owner 期间，Host 不允许 BEGIN / DATA / EXEC_START / FOCUS_ON。

安全类命令：

```text
STOP
ABORT
FOCUS_OFF
```

规则：

- 允许非 owner 触发。
- 必须由 RX 最终确认和执行。
- UI 不得本地假设安全动作已经完成。

### 5.3 UI 确认原则

Screen 点击按钮后只能进入请求态：

```text
REQUESTING_START
REQUESTING_STOP
REQUESTING_ABORT
REQUESTING_FOCUS_ON
REQUESTING_FOCUS_OFF
```

禁止：

- 本地点击 START 后直接显示 `RUNNING`。
- 本地点击 FOCUS 后直接显示 `FOCUS_ON`。
- 本地点击 STOP 后直接假设 RX 已停。

必须：

- 收到 TX/RX 回包后再确认状态。
- RX 状态优先于本地 UI 状态。
- timeout 时显示失败或未知，不显示成功。

## 6. 页面结构修正

推荐页面结构：

### 6.1 Dashboard

首页，任何模式下都可用。

显示：

```text
当前控制源：NONE / HOST / SCREEN
当前模式：IDLE / ONLINE / OFFLINE
RX 状态
任务状态
进度
关键安全按钮
```

### 6.2 Online Monitor

Host 控制时的主监控页。

显示：

```text
PC 联机监控
Host job progress
RX executing status
executed lines
cache free
STOP / ABORT / FOCUS_OFF
DONE / ERROR 提示
```

### 6.3 Offline File Browser

离线文件浏览入口。

当前阶段只做 UI 壳，不接真实 SD。

显示：

```text
SD 状态
文件列表占位
文件大小
更新时间
```

### 6.4 Job Detail

离线文件详情页。

显示：

```text
文件名
文件大小
预计行数/耗时（未来）
校验状态（未来）
发送按钮（未来）
```

### 6.5 Job Monitor

统一任务监控页，可服务 Host Online 和 Panel Offline。

显示：

```text
发送进度
接收进度
执行进度
executed lines
cache free
当前 owner
```

### 6.6 Control Panel

本地控制页。

显示：

```text
FOCUS_ON
FOCUS_OFF
低功率调焦值
安全确认
STOP / ABORT 快捷入口
```

权限：

- Host 执行中禁用 `FOCUS_ON`。
- `FOCUS_OFF` 始终可用。

### 6.7 Diagnostics

诊断页。

显示：

```text
Host connected
TX connected
RX connected
SLE link
SD mounted
last error
heap
fps
firmware version
```

### 6.8 Settings

设置页。

包含：

```text
亮度
提示音开关（未来）
主题信息
关于设备
```

### 6.9 Hidden Easter Egg Games

隐藏彩蛋页，最低优先级。

原则：

- 只在 idle/waiting 场景可进入。
- 任意安全事件、执行事件、错误事件必须立即退出或暂停。
- 不得干扰 STOP / ABORT / FOCUS_OFF。

## 7. MVP 优先级修正

### Phase A：fake model UI 基础

范围：

```text
Dashboard
Online Monitor 壳
Offline File Browser 壳
Settings
Diagnostics
```

不接：

- TX
- RX
- Host
- SD
- SLE
- UART
- 音频
- 小游戏

目标：

- 证明屏幕作为 HMI 的信息架构成立。
- 继续收敛显示和触摸稳定性。

### Phase B：Host Online Monitor 假数据

目标：

- 用 fake model 验证“上位机控制时屏幕有用”。
- 显示 control source = HOST。
- 显示 mode = ONLINE。
- 显示 RX state / progress / executed lines / cache free。
- STOP / ABORT / FOCUS_OFF 仍为 UI 请求态，不接真实链路。

### Phase C：SD 文件浏览 UI

目标：

- 完成 Offline File Browser 页面。
- 使用 fake 文件列表。
- 不接真实 SD。

### Phase D：Offline Job Client fake transport

目标：

- 做 Screen 离线 job 发送流程的 fake transport。
- 验证 owner = SCREEN 的 UI 状态变化。
- 不接真实 SLE。

### Phase E：真实 Screen -> RX 离线链路

长期目标。

范围：

- SD read。
- SLE Job Client。
- RX ACK/retry/timeout。
- owner lock。

该阶段不属于当前任务。

### Phase F：Host 模式真实状态镜像

长期目标。

范围：

- Screen 获取 Host/TX/RX 状态。
- 显示真实 Online Monitor。
- 安全命令接入 TX/RX。

该阶段不属于当前任务。

### Phase G：完成提示音

长期目标。

范围：

- 短提示音。
- 错误提示音。
- 不实现音乐播放器。

该阶段不属于当前任务。

### Phase H：隐藏小游戏

最低优先级。

范围：

- 隐藏入口。
- idle/waiting 可用。
- 安全事件立即退出。

该阶段不属于当前任务。

## 8. 与 Screen-TX 接入计划的关系

`screen_tx_integration_plan.md` 定义了第一条现实接入路线：Host 和 Screen 都接入 TX，由 TX 仲裁，再通过 SLE Job Packet 接 RX。

`communication_modes.md` 进一步修正最终通信判断：

- Host Online Mode：任务数据仍走 `Host UI -> TX -> RX`，Screen 通过 `TX -> Screen` 状态镜像成为状态显示器、本地安全面板、完成提醒器和诊断显示面板。
- Panel Offline Mode：Screen 走 `Screen -> SD read -> SLE Job Client -> RX`，TX 可以断电，Screen 取代 TX 的生态位成为离线模式下的 job owner。
- 状态适合镜像转发；任务数据和 ACK/NACK 推进逻辑必须点对点归属当前 owner。
- RX 是唯一执行端和状态真相源，并最终维护或确认 `control_owner = NONE / HOST / SCREEN`。

本文档补充的是产品定位和功能分层：

- Host Online Mode 下，Screen 是 HMI，不是摆设。
- Panel Offline Mode 下，Screen 是离线任务主控。
- 两种模式最终应由 `control_owner` 统一约束。

短期实现仍建议按保守顺序推进：

```text
先做任何时候都有用的 HMI
再做离线时能独立工作的 Job Controller
```

## 9. 明确结论

Screen 不应被降级为只在离线模式才工作的第二块 TX。

Screen 的正确定位是设备本体工业 HMI：

- Host 控制时：负责监控、安全、提醒和展示。
- 离线模式时：负责 SD 文件选择、SLE 下发和执行监控。
- 空闲待机时：负责设备状态、设置、诊断和产品化呈现。

最终一句话架构：

```text
TX 是电脑模式的无线入口；
Screen 是设备本体 HMI，并在离线模式下升级为无线任务入口；
RX 是唯一执行端和状态真相源。
```

后续所有 UI、协议、页面和状态机设计都应围绕这个双角色定位展开。
