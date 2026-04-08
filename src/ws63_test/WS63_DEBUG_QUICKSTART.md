# WS63 激光打标机：傻瓜式上手与 Debug 手册

配套文档：
- 使用与交付入口：[README.md](./README.md)
- 源码导读与模块职责：[CODE_ARCHITECTURE.md](./CODE_ARCHITECTURE.md)

适用目录：`ws63_test/`

## 0. 当前状态结论（稳定基线）

已修复：
1. 文档与默认硬件配置已对齐（`GPIO10/PWM2/UART1`）。
2. 发射板新增参考官方例程实现的 `WiFi SoftAP + TCP` 文本入口，且不影响原串口基线。
3. 接收侧新增命令参数校验（坐标范围、速度有限值、非法命令拒收）。
4. `ack_seq` 语义与注释已统一为“已接收(入队)最新序号”。
5. `Kconfig` 与 `common/config.h` 已打通（关键总线/通道/CS 可由 `CONFIG_*` 覆盖）。
6. 发射板首条业务命令不再使用 `seq=0`，避免初始 `ack=0` 误判。
7. 发射板心跳会给业务命令让路，降低 SSAP 在途窗口争抢。
8. 接收板 `Run/Idle` 判定已修正，不再把“激光开但不动”长期报成 `Run`。
9. 接收板安全监控已改为连续超时确认，避免单次抖动直接停光。

当前回归结果：
1. `smoke` 通过。
2. `square` 通过。
3. `repeat --rounds 20` 通过。
4. `stress_test.py --suite repeat --rounds 20 --cycles 50` 已完成 `50/50 PASS`。

仍可继续优化：
1. 发射侧 `ready` 目前仍以 `CMD handle` 为最小条件；状态句柄异常时，流控会退化。

## 1. 5 分钟上手（按这个来，别跳步）

### 第 1 步：选构建目标
在仓库根目录执行：

```bash
python3 build.py menuconfig ws63-liteos-app
```

在菜单里只开一个目标（避免混编干扰）：
- 接收板：`CONFIG_LASER_MARKER_RECEIVER=y`，`CONFIG_LASER_MARKER_TRANSMITTER` 关闭
- 发射板：`CONFIG_LASER_MARKER_TRANSMITTER=y`，`CONFIG_LASER_MARKER_RECEIVER` 关闭

说明：当前环境中已确认接收板配置曾被启用。

### 第 2 步：编译

```bash
python3 build.py ws63-liteos-app
```

常用变体：

```bash
python3 build.py -c ws63-liteos-app
```

### 第 3 步：找产物
默认打包产物：

```text
output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg
```

ELF（调试/符号分析）：

```text
output/ws63/acore/ws63-liteos-app/ws63-liteos-app.elf
```

### 第 4 步：烧录两块板
- 接收板先烧“接收固件”配置版本。
- 发射板再烧“发射固件”配置版本。
- 串口下载建议速率可从工程配置取 `921600`。

说明：本仓库主要提供了 Upload 产物路径，命令行烧录工具链依你当前开发环境（通常 DevEco Upload 或厂商烧录工具）。

### 第 5 步：连线与上电
以 `common/config.h` 为准：

- 接收板：`SPI0 + GPIO10(CS)` 驱动 DAC，`GPIO2/PWM2` 控激光
- 发射板：`UART1(GPIO15/16)` 接上位机，或使用其 `WiFi SoftAP + TCP` 入口

## 2. 一张图看全链路（GCode -> 振镜/激光）

```text
LaserGRBL/上位机/网页端
    |
    | UART 或 WiFi TCP 文本 GCode
    v
[发射板 uart_handler / wifi_gcode_server]
    - 按行收包
    - '?' 状态查询 / '$' 本地命令 / GCode 解析
    |
    v
[发射板 gcode_processor]
    - G/M/S/F 语义转 motion_cmd_t
    - 生成 CRC + seq
    |
    | SLE write
    v
[接收板 sle_server]
    - 长度校验
    - CRC 校验
    - 入 cmd_queue
    |
    v
[接收板 interpolator 任务]
    - pop 命令
    - perform_move() 线性插补
    - mm_to_dac() 坐标转 DAC
    |
    +--> [dac8562] SPI 输出 XY 到振镜
    |
    +--> [laser_ctrl] PWM 输出功率到激光

同时：
[接收板] 发送 status_full_pkt(队列余量/ACK/坐标) --> [发射板 sle_client]
[发射板] 用于流控 + '?' 查询回传
```

## 3. 傻瓜式 Debug（从“完全不动”一路排到“能雕刻”）

### A. 先看启动日志（最关键）

接收板应看到类似：
- `WS63 Laser Marker - Receiver Board`
- `[receiver] hardware init OK`
- `[interpolator] task started`
- `[safety] monitor task started`
- `[laser rx] SLE enable called`

发射板应看到类似：
- `WS63 Laser Marker - Transmitter Board`
- `[uart] init OK`
- `[uart] task started`
- `[laser tx] scanning...`
- 若开启 WiFi，还应看到 `[wifi gcode] wifi init ready`
- 以及 `[wifi gcode] softap ready ...`

若这一步不对，先别看协议，先修初始化。

补充说明：
- 启动时若看到 `Flash Init Fail! ret = 0x80001341`，但随后业务任务正常启动，这通常是外部 Flash 型号未命中当前表项导致的告警，不一定会阻断应用主流程。

### B. SLE 是否连上
连上后通常会出现：
- 发射板：`found LaserRX`、`connected!`
- 接收板：`SLE connected`

连不上优先检查：
- 两块固件是否烧反
- 设备名是否为 `LaserRX`
- 板卡是否都进入 SLE 初始化

典型症状：
- 发射板持续打印 `adv_report ... aa:**:**:**:ee:01`，但始终没有 `found LaserRX` 和 `connected!`。
- 这通常表示“能收到广播，但未拿到包含设备名的 scan response”。
- 处理：把发射板扫描类型设为主动扫描（`SLE_SEEK_ACTIVE`），再复位重试。

### C. 上位机到发射板 UART 通不通
- 上位机连接发射板 UART1（不是 UART0）
- 复位后应收到 `Grbl 1.1f ...`
- 输入 `?` 应返回 `<Idle|...>` 或 `<Run|...>`

Windows 现场联调默认映射可参考：
- 业务串口：`COM8`
- 发射板调试串口：`COM11`
- 接收板调试串口：`COM13`

### C2. 发射板 WiFi 通不通
- 查看发射板日志是否出现 `softap ready ssid=WS63_LaserTX ip=192.168.43.1 port=5000`
- PC 连接发射板热点后，确认拿到 `192.168.43.x` 网段地址
- 用 TCP 客户端连接 `192.168.43.1:5000`
- 建连后应立即收到：
  - `WS63 Laser Marker WiFi`
  - `Grbl 1.1f ['$' for help]`
- 之后发送 `?` 或 `$I`，应获得与串口入口一致的回复

注意：
- 当前发射板内部仍是一份共享 G-Code 状态机，建议同一时刻只使用 `UART` 或 `WiFi` 其中一个入口。
- 如果两条入口同时发业务命令，行为虽然不会破坏 SLE 协议，但会让上层坐标/模式状态变得难以预测。

### D. 指令是否进入接收队列
现象：发射板有 `ok`，但接收板不动。
排查顺序：
1. 发射板是否 `is_ready`（SLE 已连并发现服务）
2. 接收板是否打印 CRC/size 错误
3. 是否出现 `queue full`（说明插补消费跟不上）

### E. 振镜动了但激光不出光
- 检查是否有 `M3/M4`、`S` 功率是否 > 0
- 检查 `laser_ctrl` PWM 管脚与硬件一致
- 看安全任务是否触发 `SLE timeout! Laser OFF`

### F. 运行中突然停光
优先看接收板：
- 是否出现 `SLE timeout! Laser OFF` 或 `SLE timeout confirmed! ... Laser OFF`
- 若频繁触发，检查链路心跳、无线稳定性、任务阻塞
- 若日志形如 `elapsed=809 threshold=800`，说明链路抖动已经接近安全门限，优先核对 `common/config.h` 中
  `HEARTBEAT_INTERVAL_MS=200`、`SAFETY_SLE_TIMEOUT_MS=1000`、`SAFETY_SLE_TIMEOUT_ACTIVE_MS=1500`、
  `SAFETY_TIMEOUT_CONFIRM_COUNT=3`
  是否已烧录到当前接收板固件

## 4. 最小可用联调脚本（人工版）

在 LaserGRBL/串口工具逐行发：

```gcode
$I
G90
M3 S200
G1 X10 Y10 F6000
G1 X20 Y10
M5
?
```

预期：
- 每行回 `ok`
- 振镜按轨迹移动
- `M3/M5` 控制激光开关
- `?` 返回位置与状态

如果改走 WiFi，只需要把同样的文本命令发到 `192.168.43.1:5000`，其余行为与串口入口一致。

## 5. 推荐验收顺序

1. 先跑 `smoke`，确认基本链路可用。
2. 再跑 `square`，确认位置回零和多段轨迹正常。
3. 再跑 `repeat --rounds 20`，确认长链路往返稳定。
4. 最后跑 `stress_test.py --suite repeat --rounds 20 --cycles 50`，作为交付留档结果。

## 6. 如果要把问题交给 AI 排障

如果你后面准备把项目交给新的 AI 来继续排障或接手开发，建议至少上传下面这些材料：

### 最小文档集

1. [README.md](/root/fbb_ws63/src/ws63_test/README.md)
2. [CODE_ARCHITECTURE.md](/root/fbb_ws63/src/ws63_test/CODE_ARCHITECTURE.md)
3. [WS63_DEBUG_QUICKSTART.md](/root/fbb_ws63/src/ws63_test/WS63_DEBUG_QUICKSTART.md)
4. [README.md](/root/fbb_ws63/src/ws63_test/ai_studio/README.md)

### 如果是固件问题，再补这些

1. `common/config.h`
2. `common/protocol.h`
3. `transmitter/uart_handler.c`
4. `transmitter/sle_client.c`
5. `receiver/sle_server.c`
6. `receiver/safety_monitor.c`

### 如果是上位机问题，再补这些

1. `ai_studio/main_window.py`
2. `ai_studio/image_processing.py`
3. `ai_studio/ai_image_generator.py`
4. `ai_studio/serial_worker.py`
5. `ai_studio/gcode_generator.py`

### 最有价值的运行材料

- 一份失败现场日志
- 一张触发问题的原图
- 一份生成后的轮廓预览图
- 一份导出的 G-Code
- 一份当前自动测试结果或报错截图

这样新的 AI 才能快速判断问题到底在：
- AI 生图
- 图像处理
- G-Code 生成
- 串口发送
- 还是双板固件
