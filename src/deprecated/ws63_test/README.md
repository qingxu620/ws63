# WS63 激光打标双板工程 README (Deprecated)

**本模块已废弃，活跃开发请见 `src/ws63_laser_sle_job/` 和 `src/ws63_laser_rx_unified/`。**

`src/ws63_test` 是一套面向 WS63 的多板激光打标样例工程。它把“上位机 G-Code -> 发射板解析 -> SLE 无线下发 -> 接收板插补执行 -> DAC/PWM 输出 -> 状态回传”这条链路完整打通，适合做功能开发、联调验证和项目交付基线。当前发射板除了保留原来的 UART 入口，也新增了参考官方 WiFi 例程实现的 TCP 文本入口，支持 `SoftAP` 直连和 `STA` 入网两种模式，方便后续网页端或自研上位机接入。

当前目录包含五类核心工程模块：
- `common/`：协议、CRC、全局配置，负责收发两端共享契约。
- `transmitter/`：发射板固件，负责 UART/WiFi/G-Code/SLE Client。
- `receiver/`：接收板固件，负责 SLE Server/命令队列/插补/DAC/PWM/安全监控。
- `zdt_controller/`：张大头 `Emm_V5.0` 的底层驱动与 bring-up 代码目录，供 `focus_node/` 复用，不再作为独立板卡角色编译。
- `focus_node/`：感知与对焦节点固件，也是当前唯一的 `Z` 轴节点角色；第一版先整合 `TTL UART + ZDT Z轴`，为后续 `NFC / 测高 / 自动对焦` 预留统一节点入口。

配套支持目录：
- `tools/`：Windows 上位机自动化测试脚本。
- `ai_studio/`：AI 智能创作中枢 PC 上位机，负责 AI 生图、轮廓提取、G-Code 生成，以及通过串口或 WiFi 下发到发射板。
- `PROJECT_CAPABILITY_FEASIBILITY_PLAN.md`：汇总当前已实现能力、提升方向可行性与项目推进计划，适合对外说明与内部规划。
- `PROJECT_COMPETITION_UPGRADE_PLAN.md`：把现有工程底座与竞赛增强方案整合成一份更适合立项书、答辩和团队排期的总纲。
- `WS63_DEBUG_QUICKSTART.md`：统一的现场联调、安卓手机网络方案、主备切换与验收手册。
- `CODE_ARCHITECTURE.md`：代码架构梳理与源码阅读地图。
- `tools/WIFI_DEBUG_MANUAL.md`：WiFi 工具链、网页控制台与自动化脚本的专用调试手册。

## 1. 你会先用到哪些文档

如果你是第一次接手这个工程，建议按下面顺序看：

1. 先看本 README：搞清楚工程目标、目录、编译烧录、验收路径。
2. 再看 [WS63_DEBUG_QUICKSTART.md](./WS63_DEBUG_QUICKSTART.md)：按“先主方案，再备方案”的顺序做现场联调和验收。
3. 再看 [PROJECT_CAPABILITY_FEASIBILITY_PLAN.md](./PROJECT_CAPABILITY_FEASIBILITY_PLAN.md)：快速了解当前成果、扩展方向和推进计划。
4. 如果要准备比赛立项书、答辩 PPT 或团队分工，再看 [PROJECT_COMPETITION_UPGRADE_PLAN.md](./PROJECT_COMPETITION_UPGRADE_PLAN.md)：按“当前底座 + 竞赛增强 + 二期路线”统一理解项目。
5. 如果重点排查 WiFi，再看 [WIFI_DEBUG_MANUAL.md](./tools/WIFI_DEBUG_MANUAL.md)：直接使用现成工具链联调。
6. 最后看 [CODE_ARCHITECTURE.md](./CODE_ARCHITECTURE.md)：进入源码、二次开发、问题定位。

## 2. 前置知识目录

开始之前，建议至少具备下面这些基础认知：

| 主题 | 需要知道什么 | 对应位置 |
| --- | --- | --- |
| G-Code 基础 | `G0/G1/G90/G91/G92/M3/M4/M5/S/F/?/$I/$G` 的基本含义 | `transmitter/gcode_parser.c`、`transmitter/gcode_processor.c` |
| 串口通信 | 上位机通过 UART1 给发射板逐行发文本指令 | `transmitter/uart_handler.c` |
| WiFi 通信 | 发射板基于 SoftAP / STA + TCP 暴露第二条文本 G-Code 入口 | `transmitter/wifi_gcode_server.c` |
| SLE 通信 | 发射板是 Client，接收板是 Server，命令与状态通过 SSAP 特征值传输 | `transmitter/sle_client.c`、`receiver/sle_server.c` |
| 坐标与功率输出 | X/Y 由 DAC8562 输出，激光功率由 PWM 输出 | `receiver/dac8562.c`、`receiver/laser_ctrl.c` |
| 插补与执行 | 接收板消费命令队列并执行线性插补 | `receiver/interpolator.c` |
| 安全机制 | 接收板会根据心跳和业务活动做超时停光保护 | `receiver/safety_monitor.c` |
| 构建系统 | 通过 `Kconfig + CMakeLists.txt` 选择接收板、发射板、感知与对焦节点或安全节点 | `Kconfig`、`CMakeLists.txt` |
| 自动化测试 | 用 Python 串口脚本做 smoke/square/repeat/stress 回归 | `tools/uart_auto_test.py`、`tools/stress_test.py` |

## 3. 新手入门指南

### 3.1 先建立整体认知

一套完整链路里各节点的职责如下：

```text
LaserGRBL / 串口工具 / 自研 TCP 客户端
    |
    | UART1 或 WiFi TCP 文本 G-Code
    v
发射板 transmitter
    |
    | SLE motion_cmd_t
    v
接收板 receiver
    |
    +--> DAC8562 -> 振镜 X/Y
    +--> PWM      -> 激光功率
    |
    `--> status_full_pkt 回传发射板
```

要点只有两条：
- 发射板负责“理解上位机语言”。
- 接收板负责“真实执行运动和出光”。

### 3.2 先准备什么

- 一块发射板固件目标板
- 一块接收板固件目标板
- 上位机到发射板的业务串口线
- 可选的两路 UART0 调试串口
- 可运行 `python3`/`py -3` 的 PC 环境
- `pyserial` Python 依赖

Windows 现场联调常用映射：
- 业务串口：`COM8`
- 发射板调试串口：`COM11`
- 接收板调试串口：`COM13`

## 4. 目录结构

```text
src/ws63_test/
├── CMakeLists.txt
├── Kconfig
├── README.md
├── CODE_ARCHITECTURE.md
├── WS63_DEBUG_QUICKSTART.md
├── ai_studio/
│   ├── main.py
│   ├── main_window.py
│   ├── serial_worker.py
│   ├── image_processing.py
│   ├── gcode_generator.py
│   └── ai_image_generator.py
├── common/
│   ├── config.h
│   ├── protocol.h
│   ├── crc16.c
│   └── crc16.h
├── transmitter/
│   ├── main.c
│   ├── uart_handler.c
│   ├── wifi_gcode_server.c
│   ├── gcode_parser.c
│   ├── gcode_processor.c
│   └── sle_client.c
├── receiver/
│   ├── main.c
│   ├── sle_server.c
│   ├── cmd_queue.c
│   ├── interpolator.c
│   ├── dac8562.c
│   ├── laser_ctrl.c
│   └── safety_monitor.c
├── zdt_controller/
│   ├── main.c
│   ├── zdt_controller.c
│   ├── zdt_protocol.c
│   └── zdt_uart.c
├── focus_node/
│   ├── main.c
│   ├── focus_service.c
│   └── focus_service.h
└── tools/
    ├── wifi_client.py
    ├── wifi_console.py
    ├── wifi_console.html
    ├── wifi_test.py
    ├── wifi_bridge.py
    ├── uart_auto_test.py
    └── stress_test.py
```

## 5. 三个工程模块概览

### 5.1 `common` 共享契约模块

职责：
- 定义收发板共用协议结构
- 定义 CRC 校验
- 固化硬件引脚、超时、流控与安全参数

关键文件：
- `common/protocol.h`：`motion_cmd_t`、`status_pkt_t`、`status_full_pkt_t`
- `common/config.h`：UART/SLE/心跳/安全/硬件配置
- `common/crc16.c`：无线链路数据完整性校验

### 5.2 `transmitter` 发射板模块

职责：
- 接收上位机 UART1 的 G-Code 文本
- 接收上位机 WiFi TCP 的 G-Code 文本
- 做 Grbl 兼容应答
- 生成 `motion_cmd_t`
- 通过 SLE 可靠下发给接收板
- 接收接收板状态回传并用于 `?` 查询和流控

关键文件：
- `transmitter/main.c`
- `transmitter/uart_handler.c`
- `transmitter/wifi_gcode_server.c`
- `transmitter/gcode_parser.c`
- `transmitter/gcode_processor.c`
- `transmitter/sle_client.c`

### 5.3 `receiver` 接收板模块

职责：
- 作为 SLE Server 接收命令
- 做 CRC、参数、队列和安全校验
- 通过插补驱动振镜
- 通过 PWM 控制激光功率
- 周期回传状态和 ACK

关键文件：
- `receiver/main.c`
- `receiver/sle_server.c`
- `receiver/cmd_queue.c`
- `receiver/interpolator.c`
- `receiver/dac8562.c`
- `receiver/laser_ctrl.c`
- `receiver/safety_monitor.c`

## 6. 构建与烧录

### 6.1 选择构建目标

在仓库根目录执行：

```bash
python3 build.py menuconfig ws63-liteos-app
```

按角色选择：
- 接收板：打开 `CONFIG_LASER_MARKER_RECEIVER`
- 发射板：打开 `CONFIG_LASER_MARKER_TRANSMITTER`
- 感知与对焦节点：打开 `CONFIG_LASER_MARKER_FOCUS_NODE`
- 安全终端节点：打开 `CONFIG_LASER_MARKER_SAFETY_NODE`

固定硬件映射：
- 接收板固定使用 `SPI0 + GPIO10(CS) + PWM2`
- 发射板固定使用 `UART1(GPIO15/16)`

如果选择发射板，还可以继续确认 WiFi 相关选项：
- `CONFIG_LASER_WIFI_SOFTAP_ENABLE`：是否启用 WiFi 入口
- `CONFIG_LASER_WIFI_MODE_SOFTAP`：发射板自己开热点，适合比赛现场直连
- `CONFIG_LASER_WIFI_MODE_STA`：发射板连接现有路由器/手机热点，适合电脑和手机在同一局域网下访问

`SoftAP` 和 `STA` 在菜单里是二选一。

如果选择 `感知与对焦节点`，还可以继续确认：
- `CONFIG_ZDT_UART_BAUD`：张大头驱动串口波特率
- `CONFIG_ZDT_DEVICE_ADDR`：张大头驱动地址
- `CONFIG_ZDT_RS485_DIR_ENABLE`：是否启用 RS485 DE/RE 方向脚
- `CONFIG_ZDT_DEMO_AUTO_RUN`：是否上电自动执行一次安全的演示运动

当前推荐的 Z 轴接线口径：
- `Emm_V5.0` 优先走 `TTL UART` 直连 `WS63`
- `RS485 / Modbus` 优先留给后续测高传感器

### 6.2 编译

```bash
python3 build.py ws63-liteos-app
```

需要完整清理时：

```bash
python3 build.py -c ws63-liteos-app
```

### 6.3 常用产物位置

固件包：

```text
output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg
```

ELF：

```text
output/ws63/acore/ws63-liteos-app/ws63-liteos-app.elf
```

### 6.4 烧录顺序建议

1. 先烧接收板固件。
2. 再烧发射板固件。
3. 上电后先看两路 UART0 启动日志，再接上位机业务串口。

### 6.5 当前功能逐条落地顺序

如果你们已经把 `UART` 链路全部验证完成，下一阶段建议不要同时铺太多新点，直接按下面顺序把
`WiFi TCP` 能力落地。这样做的好处是：每一步都能独立验收，失败时也容易定位在哪一层。

推荐顺序：

| 步骤 | 现在要做什么 | 推荐做法 | 通过标准 |
| --- | --- | --- | --- |
| 1 | 固定 WiFi 调试模式 | 第一轮优先选 `SoftAP`，避免把问题混到路由器、DHCP、局域网里 | 发射板配置为 `CONFIG_LASER_MARKER_TRANSMITTER=y` + `CONFIG_LASER_WIFI_SOFTAP_ENABLE=y` + `CONFIG_LASER_WIFI_MODE_SOFTAP=y` |
| 2 | 只确认板端 WiFi 是否成功启动 | 上电后先只看发射板日志，不急着连 PC | 日志里出现 `wifi init ready`、`softap ready ...`、`tcp listen ready ...` |
| 3 | 只确认 PC 到发射板 TCP 能连通 | PC 连 `WS63_LaserTX`，再用 `wifi_bridge.py --host 192.168.43.1 -i` | 能看到 `WS63 Laser Marker WiFi`、`Grbl 1.1f ...` 欢迎语 |
| 4 | 确认 WiFi 入口真的打到了现有控制链路 | 先发查询命令，再发最短动作命令 | `$WIFI?` 返回 `SLE=1`，`G90` / `M5` / `?` 都能正常回复 |
| 5 | 复用串口已验证过的最小动作集 | 只发 `G90`、`G92`、`M3 S200`、`G1 X10 Y10 F6000`、`M5` | 每条返回 `ok`，接收板有动作，激光开关受控 |
| 6 | 用自动化脚本做 WiFi smoke | 先跑 `python3 src/ws63_test/tools/wifi_test.py --host 192.168.43.1 --suite smoke` | `intro_check / wifi_status / smoke` 全部 `PASS` |
| 7 | 再做稳定性和展示入口 | 跑 `--suite all --cycles 5`，然后切到 `ai_studio` 的 `WiFi TCP` 模式 | 自动化通过，AI 上位机也能通过 WiFi 成功发任务 |

额外建议：
- 第一轮先不要调 `STA`，因为 `SoftAP` 路径最短，最适合把“板端 WiFi 服务是否真跑起来”先确认掉。
- 第一轮先不要让 `UART` 和 `WiFi` 同时发业务命令，当前发射板内部仍是同一份 G-Code 状态机。
- 如果第 2 步都没过，先不要看 Python 脚本；如果第 3 步过了但第 4 步没过，优先看 `SLE` 是否连上。

建议直接用下面这组命令作为 WiFi 第一轮验收基线：

```bash
python3 src/ws63_test/tools/wifi_bridge.py --host 192.168.43.1 -i
python3 src/ws63_test/tools/wifi_test.py --host 192.168.43.1 --suite smoke
python3 src/ws63_test/tools/wifi_test.py --host 192.168.43.1 --suite all --cycles 5
```

## 7. 硬件与接口基线

以 `common/config.h` 当前配置为准：

### 接收板

- SPI：`SPI0`
- DAC CS：`GPIO10`
- SPI CLK：`GPIO7`
- SPI MOSI：`GPIO9`
- 激光 PWM：`GPIO2 / PWM2`

### 发射板

- 业务串口：`UART1`
- UART TX：`GPIO15`
- UART RX：`GPIO16`
- 波特率：`115200`
- WiFi TCP 端口：默认 `5000`
- WiFi 模式：`SoftAP` 或 `STA`，由 `menuconfig` 选择

当选择 `SoftAP` 模式时：
- WiFi 热点：默认 `WS63_LaserTX`
- WiFi 密码：默认 `ws63laser`
- WiFi 信道：默认 `13`
- WiFi IP：默认 `192.168.43.1`

补充说明：
- 若手机或电脑搜不到 `WS63_LaserTX`，优先检查当前 `SoftAP` 信道。
- 默认 `13` 信道在部分地区或部分网卡配置下可能不会显示，建议第一轮联调用 `1`、`6` 或 `11`，优先推荐 `6`。

当选择 `STA` 模式时：
- 目标热点 SSID：由 `CONFIG_LASER_WIFI_STA_SSID` 指定
- 目标热点密码：由 `CONFIG_LASER_WIFI_STA_PSK` 指定
- 发射板 IP：由目标路由器/手机热点的 DHCP 分配，以上电日志为准

使用说明：
- UART 与 WiFi 都是发射板的上游入口，二者都通向同一套 `G-Code -> motion_cmd_t -> SLE` 控制链路。
- 当前发射板内部仍然只有一份 G-Code 上下文状态机，建议同一时刻只选 `UART` 或 `WiFi` 其中一种入口，不要并发发送业务命令。

## 8. 最小联调路径

### 8.1 人工串口联调

在串口工具里向发射板 UART1 逐行发送：

```gcode
$I
G90
M3 S200
G1 X10 Y10 F6000
G1 X20 Y10
M5
?
```

预期现象：
- 每条业务命令返回 `ok`
- 接收板振镜执行轨迹
- `M3/M5` 控制激光开关
- `?` 返回 `<Idle|...>` 或 `<Run|...>`

### 8.2 WiFi TCP 联调

发射板启动后，若 WiFi 初始化正常，日志中会出现类似：

```text
[wifi gcode] wifi init ready
[wifi gcode] softap ready ssid=WS63_LaserTX ip=192.168.43.1 port=5000 channel=13
[wifi gcode] tcp listen ready mode=SoftAP ip=192.168.43.1 port=5000
```

如果你在手机或电脑上搜不到 `WS63_LaserTX`，但日志已经出现上面这几行，优先不要怀疑 WiFi 初始化失败，而是先看最后的 `channel=13`。

建议处理顺序：
- 保持 `SoftAP` 模式不变
- 在 `menuconfig` 里把 `Choose SoftAP Channel (信道)` 从 `13` 改成 `6`
- 重新编译并重新烧录发射板
- 再次确认日志变成 `channel=6`
- 然后重新在手机或电脑上搜索 `WS63_LaserTX`

如果你启用的是 `STA` 模式，日志会变成类似：

```text
[wifi gcode] wifi init ready
[wifi gcode] sta enabled, target ssid=YourRouterSSID
[wifi gcode] sta ready ssid=YourRouterSSID ip=192.168.1.123 port=5000
[wifi gcode] tcp listen ready mode=STA ip=192.168.1.123 port=5000
```

联调方式：
- `SoftAP`：PC 先连接发射板热点，再访问 `192.168.43.1:5000`
- `STA`：PC 和发射板先接入同一路由器/手机热点，再访问日志里的发射板 IP 和 `5000` 端口

可用任意行式 TCP 客户端向发射板发送和串口相同的文本 G-Code，例如：

```gcode
$I
$CAP?
$WIFI?
G90
M3 S200
G1 X10 Y10 F6000
M5
?
```

预期与 UART 一致：
- 建连后会先收到 `WS63 Laser Marker WiFi` 和 `Grbl 1.1f ['$' for help]`
- 还会看到 `[MSG:WiFi SoftAP ...]` 或 `[MSG:WiFi STA ...]`
- 每条命令回 `ok`
- `?` 返回位置与运行状态
- `$CAP?` 返回当前作品能力画像，适合比赛现场快速证明当前链路支持项
- `$WIFI?` 返回当前 WiFi 模式、IP、TCP 监听状态、客户端连接状态以及 SLE 就绪状态
- 真正的运动命令仍由发射板转成 `motion_cmd_t` 后通过 SLE 发往接收板

### 8.2.1 WiFi 小工具

`tools/` 目录额外提供了 3 个面向 WiFi TCP 入口的小工具。它们都复用同一套
`WifiGcodeClient` 传输层，并且 Python 运行时不依赖第三方包。

本地网页控制台：

```bash
python3 src/ws63_test/tools/wifi_console.py
python3 src/ws63_test/tools/wifi_console.py --web-port 9000
```

说明：
- 浏览器打开 `http://localhost:8080`（或自定义端口）
- 页面会轮询刷新运动状态和 `$WIFI?` 状态，不是一次连接后的静态缓存
- “清空日志”会同时清空页面和后端会话日志，后续轮询不会回灌旧内容
- “急停”当前发送 `!`，会走板端真急停链路，立即停光、清队列并打断当前运动

TCP 通用验证工具：

```bash
python3 src/ws63_test/tools/wifi_test.py --host 192.168.43.1 --suite smoke
python3 src/ws63_test/tools/wifi_test.py --host 192.168.43.1 --suite all --cycles 10 --report-json result.json
```

轻量桥接脚本：

```bash
python3 src/ws63_test/tools/wifi_bridge.py --host 192.168.43.1 --file demo.gcode
python3 src/ws63_test/tools/wifi_bridge.py --host 192.168.43.1 --interactive
cat commands.txt | python3 src/ws63_test/tools/wifi_bridge.py --host 192.168.43.1
```

说明：
- 交互模式默认会生成一份会话日志
- 若传 `--log-file`，则使用指定日志路径
- 文件模式和管道模式默认不自动生成日志

### 8.3 自动测试

先安装依赖：

```bash
pip install pyserial
```

如果你把 `tools/` 里的脚本单独拷到 Windows 目录运行，可以直接用下面的命令：

```powershell
py -3 .\uart_auto_test.py COM8 --tx-debug-port COM11 --rx-debug-port COM13 --suite smoke
py -3 .\uart_auto_test.py COM8 --tx-debug-port COM11 --rx-debug-port COM13 --suite square
py -3 .\uart_auto_test.py COM8 --tx-debug-port COM11 --rx-debug-port COM13 --suite repeat --rounds 20
py -3 .\stress_test.py COM8 --tx-debug-port COM11 --rx-debug-port COM13 --suite repeat --rounds 20 --cycles 50 --report-json result.json
```

如果你在仓库根目录直接运行，推荐使用下面的路径写法：

```bash
python3 src/ws63_test/tools/uart_auto_test.py /dev/ttyUSB1 --tx-debug-port /dev/ttyUSB0 --rx-debug-port /dev/ttyUSB2 --suite smoke
python3 src/ws63_test/tools/stress_test.py /dev/ttyUSB1 --tx-debug-port /dev/ttyUSB0 --rx-debug-port /dev/ttyUSB2 --suite repeat --rounds 20 --cycles 50 --report-json result.json
```

### 8.4 AI 智能创作中枢

如果你要体验“AI 生图 -> 轮廓提取 -> G-Code -> 串口或 WiFi 下发”的比赛版上位机，可先安装：

```bash
pip install PySide6 opencv-python pyserial requests numpy
```

然后在仓库根目录运行：

```bash
python3 -m src.ws63_test.ai_studio.main
```

如果现场还没准备好真实 AI 接口，这个上位机会自动生成 mock 示例图，便于先联调图像处理和打标链路。

## 9. 推荐验收顺序

建议每次交付前都按下面顺序回归：

1. `smoke`
2. `square`
3. `repeat --rounds 20`
4. `stress_test.py --suite repeat --rounds 20 --cycles 50`

当前稳定基线：
- `smoke` 通过
- `square` 通过
- `repeat --rounds 20` 通过
- `stress_test.py --suite repeat --rounds 20 --cycles 50` 已完成 `50/50 PASS`

## 10. 当前版本的关键工程结论

这版工程已经把几个最容易在联调阶段出问题的点收住了：

- 首条业务命令不再与初始 `ack=0` 冲突
- 心跳会给业务命令让路，降低 SSAP 在途窗口争抢
- 接收板 `Run/Idle` 状态语义已经修正
- 接收板安全监控采用连续超时确认，减少单次抖动误停
- 自动化脚本能处理残留 `Run`、`ok` 偶发缺失和状态查询重试

## 11. 阅读源码建议顺序

如果你要开始改代码，建议按这个顺序看：

1. `common/protocol.h`
2. `common/config.h`
3. `transmitter/main.c`
4. `transmitter/uart_handler.c`
5. `transmitter/wifi_gcode_server.c`
6. `transmitter/gcode_processor.c`
7. `transmitter/sle_client.c`
8. `receiver/main.c`
9. `receiver/sle_server.c`
10. `receiver/interpolator.c`
11. `receiver/safety_monitor.c`

## 12. 常见问题入口

- 构建目标选错：先看 `Kconfig`
- 烧录后完全不动：先看 `WS63_DEBUG_QUICKSTART.md`
- 想定位为什么某条命令没执行：先看 `CODE_ARCHITECTURE.md`
- 想验证当前固件是否还能交付：先跑 `tools/uart_auto_test.py` 和 `tools/stress_test.py`

## 13. 今日阶段结算

截至今天，这个工程已经分成两条都可工作的主线：

- 固件主线：`common + transmitter + receiver`
  - 双板无线打标闭环已稳定
  - `smoke / square / repeat / stress 50/50` 已通过
- 上位机主线：`ai_studio`
  - AI 生图 / 本地图导入 / 轮廓提取 / G-Code / 串口或 WiFi 下发 已打通
  - GUI 已完成比赛版连接切换与一键流程改造
  - 左侧工作流已改成三步骤标签页
  - 中文路径图片读取已修复
  - 设备未连接误发送已修复

当前最适合的工作方式是：
- 固件层保持“稳定基线”，谨慎改动协议和安全逻辑
- 上位机层继续迭代展示效果、图像处理效果和交互体验

## 14. 发给下一个 AI 的最小文档包

如果你后面要把项目交给新的 AI，建议最少上传下面 4 份文档：

1. [README.md](/root/fbb_ws63/src/ws63_test/README.md)
   作用：给它整个项目的总览、目录、构建方式和当前结论。
2. [CODE_ARCHITECTURE.md](/root/fbb_ws63/src/ws63_test/CODE_ARCHITECTURE.md)
   作用：让它快速知道核心源码在哪、数据怎么流、问题该从哪层查。
3. [WS63_DEBUG_QUICKSTART.md](/root/fbb_ws63/src/ws63_test/WS63_DEBUG_QUICKSTART.md)
   作用：让它理解你们现场怎么联调、怎么复现实机问题。
4. [README.md](/root/fbb_ws63/src/ws63_test/ai_studio/README.md)
   作用：让它直接接管 AI 上位机部分，知道当前阶段、风险点和下一步。

如果你希望新 AI 直接开始改代码，再额外补这 5 个源码文件最有帮助：

1. [main_window.py](/root/fbb_ws63/src/ws63_test/ai_studio/main_window.py)
2. [ai_image_generator.py](/root/fbb_ws63/src/ws63_test/ai_studio/ai_image_generator.py)
3. [image_processing.py](/root/fbb_ws63/src/ws63_test/ai_studio/image_processing.py)
4. [gcode_generator.py](/root/fbb_ws63/src/ws63_test/ai_studio/gcode_generator.py)
5. [serial_worker.py](/root/fbb_ws63/src/ws63_test/ai_studio/serial_worker.py)
