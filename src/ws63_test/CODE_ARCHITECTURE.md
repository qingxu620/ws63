# WS63 代码架构梳理

这份文档面向开发者，目标不是教你怎么第一次跑起来，而是帮你快速建立“代码在哪、数据怎么流、线程如何协作、问题该去哪里查”的脑图。

## 1. 架构总览

工程由三个核心模块组成：

1. `common`：协议和配置层，决定两块板如何“说同一种话”。
2. `transmitter`：发射板控制层，负责把上位机 G-Code 转成无线命令。
3. `receiver`：接收板执行层，负责运动、激光、安全与状态回传。

`tools` 不属于固件模块，但它是交付和回归的重要支撑层。

整体链路如下：

```text
PC / LaserGRBL / 自研 TCP 客户端 / 网页端
  -> UART1 或 WiFi TCP 文本命令
  -> transmitter/uart_handler 或 transmitter/wifi_gcode_server
  -> transmitter/gcode_processor
  -> common/motion_cmd_t
  -> transmitter/sle_client
  -> receiver/sle_server
  -> receiver/cmd_queue
  -> receiver/interpolator
  -> receiver/dac8562 + receiver/laser_ctrl
  -> receiver/sle_server 状态回传
  -> transmitter/sle_client 缓存状态
  -> transmitter/uart_handler 响应 '?'
```

## 2. 模块 1：`common` 共享协议与配置

`common` 的价值是“共享契约”。只要这一层定义不一致，发射板和接收板就会立刻失配。

### 2.1 文件职责

| 文件 | 作用 |
| --- | --- |
| `common/protocol.h` | 定义命令包、状态包、状态码、错误码、SLE UUID |
| `common/config.h` | 定义硬件引脚、默认速度、心跳、安全超时、流控参数 |
| `common/crc16.c` | 为命令和状态包提供 CRC16 校验 |

### 2.2 最重要的数据结构

#### `motion_cmd_t`

发射板下发到接收板的统一命令格式，字段包括：
- `cmd`：命令类型，如 `CMD_G1_MOVE`、`CMD_LASER_ON`
- `flags`：绝对/相对坐标、激光状态
- `seq`：命令序号，用于 ACK 追踪
- `target_x/target_y`：目标坐标
- `feed_rate`：速度
- `laser_pwr`：激光功率
- `crc16`：包校验

#### `status_pkt_t` / `status_full_pkt_t`

接收板回给发射板的状态快照，用来做：
- `ack_seq`：告诉发射板哪条命令已入队
- `queue_free`：告诉发射板接收队列还剩多少空间
- `status`：`Idle / Running / Error`
- `cur_x / cur_y`：给 `?` 状态查询和自动测试使用

### 2.3 这层最敏感的参数

`common/config.h` 里几个配置最容易影响联调稳定性：
- `SAFETY_SLE_TIMEOUT_MS`
- `SAFETY_SLE_TIMEOUT_ACTIVE_MS`
- `SAFETY_TIMEOUT_CONFIRM_COUNT`
- `HEARTBEAT_INTERVAL_MS`
- `HEARTBEAT_STATUS_REPORT_INTERVAL_MS`
- `SLE_TX_MAX_PENDING_WRITES`
- `SLE_TX_HEARTBEAT_MAX_PENDING`
- `SLE_TX_BUSINESS_MAX_PENDING`

工程调参时，优先想清楚这些参数会影响哪个时序，不要把它们当作独立常量看待。

## 3. 模块 2：`transmitter` 发射板控制层

发射板的角色不是执行器，而是“无线桥接控制器”。它一边面对上位机的串口/WiFi 文本协议，一边面对接收板的无线协议。

### 3.1 发射板任务模型

发射板由四个任务组成：

| 任务 | 入口 | 作用 |
| --- | --- | --- |
| UART 接收任务 | `task_uart_rx_entry()` | 读取 UART1 文本命令并处理 |
| WiFi 接收任务 | `task_wifi_gcode_entry()` | 按配置拉起 SoftAP 或 STA，并监听 TCP 文本命令 |
| SLE 初始化任务 | `sle_init_task()` | 扫描、连接、服务发现 |
| 心跳任务 | `heartbeat_task()` | 周期发送 `CMD_HEARTBEAT` 保活 |

主入口在 `transmitter/main.c` 中创建这些任务。

### 3.2 发射板主链路

#### 第 1 段：上位机命令进入 UART / WiFi TCP

入口文件：`transmitter/uart_handler.c`
补充入口：`transmitter/wifi_gcode_server.c`

职责：
- 初始化 UART1 引脚与驱动
- 参考官方 SoftAP / STA 例程拉起发射板热点或连接现有路由器
- 通过 TCP 按行接收 G-Code
- 处理 `?` 实时状态查询
- 处理 `$WIFI?` 链路状态查询
- 处理 `$I` / `$G` 等本地 Grbl 命令
- 对真实业务命令执行“解析 -> 下发 -> 等 ACK -> 回复 ok”

这里的关键思想是：
- 无论来自 UART 还是 WiFi，发射板都不是收到一行就立刻回 `ok`
- 而是尽量等到接收板确认入队之后再回 `ok`

这样做的好处是上位机更容易获得可靠反馈，代价是链路拥塞时 `ok` 会变慢。

当前实现还有一个边界需要明确：
- 发射板内部的 G-Code 上下文仍是一份共享状态机。
- 因此当前更适合“同一时刻只选一个上游入口”。
- 如果未来需要做 UART 与 WiFi 并发主机控制，建议再抽出统一的入口仲裁层。

当前 WiFi 模块还额外做了两件更偏工程化的事情：
- 统一注册 WiFi 事件回调，记录 SoftAP 客户端接入/离开、STA 连接/断开。
- 对外暴露 `wifi_gcode_server_get_status()` 快照接口，方便后续网页端、BLE 助手或调试界面直接复用。

#### 第 2 段：G-Code 解析与命令组帧

入口文件：
- `transmitter/gcode_parser.c`
- `transmitter/gcode_processor.c`

职责分工：
- `gcode_parser.c`：做纯文本解析，提取 `G/M/S/F/X/Y`
- `gcode_processor.c`：维护控制上下文，转成 `motion_cmd_t`

这里维护了几类发射板本地状态：
- 当前进给速度
- 当前激光功率
- 当前激光开关状态
- 当前绝对/相对坐标模式
- 当前预测位置
- 当前命令序号 `seq`

设计重点：
- `seq=0` 被保留为“尚无有效 ACK”的哨兵值
- 第一条真实业务命令从 `seq=1` 开始
- 接收板状态回传会覆盖本地预测位置

#### 第 3 段：SLE Client 无线侧

入口文件：`transmitter/sle_client.c`

职责：
- 主动扫描接收板 `LaserRX`
- 建立连接
- 做 MTU 交换和服务发现
- 找到命令特征值与状态特征值
- 发送 `motion_cmd_t`
- 接收 `status_full_pkt_t`
- 维护流控和状态快照

这层现在的关键工程策略有：
- 业务命令与心跳共享 SSAP 写请求窗口
- 但心跳配额被单独限制，避免挤占业务窗口
- 业务命令刚成功发送后，心跳会短暂让路
- 发射板根据 `queue_free` 做下发节流

### 3.3 发射板源码阅读顺序

建议按下面顺序看：

1. `transmitter/main.c`
2. `transmitter/uart_handler.c`
3. `transmitter/wifi_gcode_server.c`
4. `transmitter/gcode_processor.c`
5. `transmitter/sle_client.c`
6. `transmitter/gcode_parser.c`

### 3.4 发射板典型问题与定位入口

| 现象 | 先看哪里 |
| --- | --- |
| 串口有输入但没回 `ok` | `uart_handler.c` |
| WiFi 已连上但没回 `ok` | `wifi_gcode_server.c` |
| `?` 一直读不到状态 | `uart_handler.c` + `sle_client.c` |
| 发射板显示已连接但命令不执行 | `sle_client.c` + `receiver/sle_server.c` |
| `busy` / `pending` 飙高 | `sle_client.c` 心跳与业务写请求控制 |

## 4. 模块 3：`receiver` 接收板执行层

接收板是真正控制执行器的地方。它既要接包、验包，也要做实时运动、出光控制和安全停光。

### 4.1 接收板任务模型

接收板由三个主要任务组成：

| 任务 | 入口 | 作用 |
| --- | --- | --- |
| 插补任务 | `task_interpolator_entry()` | 消费命令队列并执行运动/激光命令 |
| 安全任务 | `task_safety_entry()` | 监控链路超时并停光 |
| SLE 初始化任务 | `sle_init_task()` | 初始化无线服务端 |

主入口在 `receiver/main.c`。

### 4.2 接收板数据入口

入口文件：`receiver/sle_server.c`

核心流程：

1. 收到发射板写请求
2. 校验包长
3. 校验 CRC
4. 校验命令参数是否合法
5. 更新安全监控时间戳
6. 心跳包不入队，只更新活跃状态
7. 业务命令入 `cmd_queue`
8. 发送状态回包，更新 `ack_seq`

这里的职责非常集中，所以它也是最关键的入口文件。

### 4.3 命令队列

入口文件：`receiver/cmd_queue.c`

设计方式：
- 环形缓冲区
- `mutex` 保护读写
- `semaphore` 唤醒消费者

它只负责“缓存命令”，不理解命令语义。

### 4.4 插补执行

入口文件：`receiver/interpolator.c`

职责：
- 从队列中取出 `motion_cmd_t`
- 对 `G0/G1` 做线性插补
- 把 mm 坐标转成 DAC 数值
- 调用 `dac8562_write_xy()` 输出到振镜
- 根据 `M3/M5` 控制激光开关和功率

几个关键点：
- 当前位置用 `g_current_x / g_current_y` 保存
- `STEP_NUM` 决定最小插补步长
- 通过 `INTERP_UNLOCK_INTERVAL` 周期让出 CPU，避免长轨迹阻塞其他任务

### 4.5 硬件驱动层

#### `receiver/dac8562.c`

职责：
- 初始化 SPI0
- 手动控制 DAC 片选
- 发送 24 bit DAC 指令
- 输出 X/Y 双通道数据

#### `receiver/laser_ctrl.c`

职责：
- 初始化 PWM 引脚
- 根据 `laser_pwr` 设置占空比
- 控制激光启停

### 4.6 安全监控

入口文件：`receiver/safety_monitor.c`

职责：
- 跟踪最近一次 SLE 接收时间
- 跟踪最近一次命令活动时间
- 区分空闲期和运动/出光期的超时门限
- 连续多次超时后关闭激光并清空命令队列

这层的设计重点：
- 使用 `uint32_t` 毫秒时间戳，避免 32 位核上读写 `uint64_t` 的撕裂风险
- 采用 `SAFETY_TIMEOUT_CONFIRM_COUNT` 连续确认，不因单次抖动误停
- 超时后执行 `laser_enable(false)` + `cmd_queue_flush()`

### 4.7 接收板源码阅读顺序

建议按下面顺序看：

1. `receiver/main.c`
2. `receiver/sle_server.c`
3. `receiver/cmd_queue.c`
4. `receiver/interpolator.c`
5. `receiver/dac8562.c`
6. `receiver/laser_ctrl.c`
7. `receiver/safety_monitor.c`

### 4.8 接收板典型问题与定位入口

| 现象 | 先看哪里 |
| --- | --- |
| 命令收到了但队列不进 | `sle_server.c` |
| `CRC error` / `invalid cmd` | `protocol.h`、`crc16.c`、`sle_server.c` |
| 振镜不动 | `interpolator.c`、`dac8562.c` |
| 激光不出光 | `laser_ctrl.c`、`safety_monitor.c` |
| 经常 `SLE timeout` | `safety_monitor.c`、发射板 `sle_client.c` |

## 5. 支撑层：`tools` 自动化测试

`tools` 不进固件，但它决定了这个工程能不能高效落地。

### 5.1 `tools/uart_auto_test.py`

用途：
- 对发射板 UART1 做脚本化串口测试
- 支持 `smoke`、`square`、`repeat`
- 支持业务串口 + 调试串口联动
- 支持 JSON 报告

脚本内部关键能力：
- 状态查询 `?` 自动重发
- 可识别残留 `Run` 状态并尝试恢复
- `ok` 偶发缺失时允许“看见实际运动后继续”

### 5.2 `tools/stress_test.py`

用途：
- 对 `uart_auto_test.py` 做多轮封装
- 适合 soak test 和交付留档
- 失败时自动打印最近调试串口日志

## 6. 关键时序与协作关系

### 6.1 一条 `G1` 的完整生命周期

```text
PC 发出 G1
 -> uart_handler 按行接收
 -> gcode_processor 组 motion_cmd_t(seq, crc)
 -> sle_client 发送到接收板
 -> sle_server 验包并入队
 -> sle_server 回 status(ack_seq, queue_free)
 -> uart_handler 等 ACK 成功
 -> UART 回 ok
 -> interpolator 取队列并执行
 -> sle_server 周期回状态
 -> PC 查询 '?' 时看到实时位置
```

### 6.2 心跳为什么重要

心跳不是为了运动控制，而是为了安全监控和链路保活：
- 接收板安全线程需要知道“无线链路还活着”
- 心跳过少会误触发停光
- 心跳过多会跟业务命令抢无线窗口

所以当前实现用了“心跳让业务”的策略，而不是简单把心跳频率拉高。

## 7. 当前版本最值得记住的工程经验

这套工程能稳定的关键，不是某一个参数，而是下面几个语义对齐：

- `ack_seq` 代表“接收板已接收并入队的最新命令”
- `Run/Idle` 只反映运动与队列状态，不等于激光使能状态
- 首条业务命令不能使用 `seq=0`
- 心跳必须保活，但不能占满业务发送窗口
- 安全监控要防真失联，也要抑制单次抖动误判

## 8. 如果你要继续二次开发

### 适合扩展的点

- 在 `gcode_processor.c` 增加更多 G/M 指令支持
- 在 `sle_server.c` 增加更细粒度错误回报码
- 在 `interpolator.c` 引入更复杂轨迹规划
- 在 `tools/` 增加更贴合工艺场景的回归脚本

### 改动时最需要小心的点

- 改 `protocol.h` 后要同时检查收发两端
- 改 `config.h` 的超时或窗口参数时，要连带考虑安全线程和无线侧
- 改 `uart_handler.c` 的 `ok` 策略时，要注意上位机兼容性
- 改 `safety_monitor.c` 时，要优先守住安全边界，不要只追求“压测更稳”

## 9. 新增上位机层：`ai_studio`

目前项目已经不再只有固件和测试脚本，还多了一条面向比赛展示和教学演示的 PC 上位机主线：

```text
Prompt / 本地图
  -> ai_studio/ai_image_generator.py
  -> ai_studio/image_processing.py
  -> ai_studio/gcode_generator.py
  -> ai_studio/serial_worker.py
  -> WS63 发射板 UART1
```

这层的定位不是替代固件，而是：
- 降低使用门槛
- 提升演示效果
- 为后续 Web / 移动端适配打基础

当前 `ai_studio` 的关键文件是：
- `main_window.py`：UI 和工作流编排
- `ai_image_generator.py`：Imagen 4 Fast 生图
- `image_processing.py`：图片转轮廓
- `gcode_generator.py`：轮廓转 G-Code
- `serial_worker.py`：串口发送和 `ok` 流控

## 10. 给后续 AI 的源码接管建议

如果后续 AI 要接管整个项目，不建议一上来就读所有源码。  
更高效的顺序是：

1. 先读 [README.md](/root/fbb_ws63/src/ws63_test/README.md)
2. 再读 [README.md](/root/fbb_ws63/src/ws63_test/ai_studio/README.md)
3. 然后读本文件建立固件脑图
4. 最后根据任务只钻对应代码层

如果任务偏固件联调，优先读：
- `common/protocol.h`
- `transmitter/uart_handler.c`
- `transmitter/sle_client.c`
- `receiver/sle_server.c`
- `receiver/safety_monitor.c`

如果任务偏上位机体验，优先读：
- `ai_studio/main_window.py`
- `ai_studio/image_processing.py`
- `ai_studio/ai_image_generator.py`
