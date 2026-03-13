# WS63 星闪激光打标机（`ws63_test`）

基于 WS63 双板架构的激光打标示例工程：
- 发射板：接收上位机 GCode（UART）并通过 SLE 下发运动命令
- 接收板：接收命令后本地插补，驱动振镜 DAC 与激光 PWM

## 1. 系统架构

```text
LaserGRBL/上位机
   │ UART(GCode)
   ▼
发射板(WS63, SLE Client)
   │ SLE motion_cmd_t
   ▼
接收板(WS63, SLE Server)
   ├─ 插补 -> DAC8562(XY振镜)
   └─ 功率 -> PWM(激光)
```

## 2. 目录结构

```text
ws63_test/
├── common/
│   ├── config.h         # 全局配置(引脚/速率/超时)
│   ├── protocol.h       # motion/status 协议
│   └── crc16.h/c        # CRC16-CCITT
├── transmitter/
│   ├── main.c
│   ├── uart_handler.h/c # UART收包 + Grbl兼容回复
│   ├── gcode_parser.h/c # 纯C GCode解析
│   ├── gcode_processor.h/c # GCode -> motion_cmd_t
│   └── sle_client.h/c   # SLE扫描/连接/发送/状态接收
├── receiver/
│   ├── main.c
│   ├── sle_server.h/c   # SLE服务与命令入口
│   ├── cmd_queue.h/c    # 线程安全命令队列
│   ├── interpolator.h/c # 线性插补核心
│   ├── dac8562.h/c      # XY DAC 驱动
│   ├── laser_ctrl.h/c   # 激光PWM控制
│   └── safety_monitor.h/c # 安全监控(超时停光)
├── Kconfig
├── CMakeLists.txt
└── README.md
```

## 3. 关键配置（以 `common/config.h` 为准）

### 接收板硬件映射
- SPI: `DAC_SPI_BUS=0`
- DAC 时钟/数据: `GPIO7(SCK)` / `GPIO9(MOSI)`
- DAC 片选: `GPIO10`
- 激光 PWM: `PWM2`, `GPIO2`

### 发射板硬件映射
- 上位机串口: `UART1`
- TX/RX: `GPIO15` / `GPIO16`
- 波特率: `115200`

### Menuconfig 可覆盖项
以下参数可由 menuconfig 的 `CONFIG_*` 覆盖默认值：
- `DAC_SPI_BUS` <- `CONFIG_LASER_DAC_SPI_BUS`
- `DAC_CS_PIN` <- `CONFIG_LASER_DAC_CS_PIN`
- `LASER_PWM_CHANNEL` <- `CONFIG_LASER_PWM_CHANNEL`
- `LASER_UART_BUS` <- `CONFIG_LASER_UART_BUS`

### 其他关键参数
- 队列深度: `CMD_QUEUE_SIZE=32`
- 心跳周期: `HEARTBEAT_INTERVAL_MS=200`
- 空闲安全超时: `SAFETY_SLE_TIMEOUT_MS=1000`
- 运动安全超时: `SAFETY_SLE_TIMEOUT_ACTIVE_MS=1500`
- 连续超时确认: `SAFETY_TIMEOUT_CONFIRM_COUNT=3`

### 当前推荐稳定基线
- 发射板业务命令 `seq` 从 `1` 开始，避免初始 `ack=0` 误判首条命令已确认
- 发射板心跳对业务命令让路，避免 SSAP 在途窗口被保活流量抢满
- 接收板 `Run/Idle` 只反映“是否仍在运动/排队”，不把激光使能本身等同于 `Run`
- 接收板安全监控采用连续超时确认，不再因单次调度抖动立即停光

## 4. 编译方式

在仓库根目录执行。

### 4.1 打开 menuconfig

```bash
python3 build.py menuconfig ws63-liteos-app
```

在菜单中进入用户样例，选择其一：
- `CONFIG_LASER_MARKER_RECEIVER=y`（接收板固件）
- `CONFIG_LASER_MARKER_TRANSMITTER=y`（发射板固件）

建议一次只编译一个角色，避免混淆。

### 4.2 编译

```bash
python3 build.py ws63-liteos-app
```

清理后编译：

```bash
python3 build.py -c ws63-liteos-app
```

### 4.3 产物位置
- 固件包：`output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg`
- ELF：`output/ws63/acore/ws63-liteos-app/ws63-liteos-app.elf`

## 5. 烧录与运行

1. 先编译并烧录接收板固件。
2. 再编译并烧录发射板固件。
3. 接收板接 DAC 与激光控制硬件，发射板接上位机串口。
4. 打开串口日志，确认两端启动成功并完成 SLE 连接。

> 上传/烧录通常可使用 DevEco Upload 或本地配套烧录工具，核心是烧录上面的 `*_all.fwpkg` 产物。

## 6. 最小联调命令（上位机）

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
- 每行收到 `ok`
- 振镜按轨迹运动
- `M3/M5` 控制激光开关
- `?` 返回位置和状态

## 7. 端到端工作流

1. 上位机通过 UART 发送 GCode 文本到发射板。
2. 发射板 `uart_handler` 按行收包并区分 `?` / `$` / 普通 GCode。
3. `gcode_processor` 把 G/M/S/F 语义转换成 `motion_cmd_t`。
4. `sle_client` 下发到接收板 `sle_server`。
5. 接收板完成长度/CRC检查后入 `cmd_queue`。
6. `interpolator` 从队列取命令执行线性插补。
7. XY 通过 `dac8562` 输出到振镜；激光功率通过 `laser_ctrl` PWM 输出。
8. 接收板回传 `status_full_pkt`（状态、队列余量、坐标），发射板据此流控与状态展示。

## 8. 说明与已知边界

- 当前已对负坐标做下限保护（接收侧）。
- 接收侧已增加入队前参数校验（坐标范围/速度有限值/非法命令拒收）。
- `ack_seq` 语义为“接收板已接收(入队)最新序号”，不是执行完成序号。
- 仍建议后续增强：当状态通知链路异常时，发射侧 `ready` 判定可进一步收紧。

## 9. 已验证结果

在当前推荐参数下，已完成以下回归：

- `smoke` 通过
- `square` 通过
- `repeat --rounds 20` 通过
- `stress_test.py --suite repeat --rounds 20 --cycles 50`：`50/50 PASS`

Windows 联调实测基线：

- 业务串口：`COM8`
- 发射板调试串口：`COM11`
- 接收板调试串口：`COM13`

## 10. 调试文档

详见：`ws63_test/WS63_DEBUG_QUICKSTART.md`

该文档包含：
- 傻瓜式排障顺序
- 常见故障定位
- 全链路流程图与检查点

## 11. Python 自动测试

如果你已经把双板链路跑通，后续回归测试建议优先用 Python 脚本，而不是继续依赖图形串口助手的人手点击。

脚本位置：

- `ws63_test/tools/uart_auto_test.py`

依赖：

```bash
pip install pyserial
```

推荐用法：

1. 只接发射板 `UART1`：

```bash
python3 ws63_test/tools/uart_auto_test.py /dev/ttyUSB1 --no-tx-debug --no-rx-debug --suite smoke
```

2. 同时接发射板 `UART1`、发射板 `UART0` 和接收板 `UART0`：

```bash
python3 ws63_test/tools/uart_auto_test.py /dev/ttyUSB1 --tx-debug-port /dev/ttyUSB0 --rx-debug-port /dev/ttyUSB2 --suite all --report-json /tmp/ws63_test.json
```

3. Windows 复制脚本后直接联调：

```bash
py -3 .\uart_auto_test.py COM8 --tx-debug-port COM11 --rx-debug-port COM13 --suite repeat --rounds 20
```

说明：

1. `UART1` 是业务串口，脚本通过它发送 `$I`、`G90`、`G1`、`?` 等命令。
2. 发射板/接收板 `UART0` 是可选调试串口，接上后脚本会等待 `status link ready` 或 `heartbeat rx=` 再开始测试，更稳。
3. 当前内置三个测试套件：
   - `smoke`
   - `square`
   - `repeat`
4. 推荐先跑 `smoke`，再跑 `square`，最后跑 `repeat` 或 `all`。
5. 可选 `--report-json /tmp/ws63_test.json` 导出机器可读测试报告，便于留档。

如果需要更长时间的稳定性验证，建议再使用：

- `ws63_test/tools/stress_test.py`

示例：

```bash
python3 ws63_test/tools/stress_test.py /dev/ttyUSB1 --suite repeat --rounds 20 --cycles 50 --report-json /tmp/ws63_stress.json
```

Windows 示例：

```bash
py -3 .\stress_test.py COM8 --tx-debug-port COM11 --rx-debug-port COM13 --suite repeat --rounds 20 --cycles 10 --report-json result.json
```

推荐验收顺序：

1. `py -3 .\uart_auto_test.py COM8 --tx-debug-port COM11 --rx-debug-port COM13 --suite smoke`
2. `py -3 .\uart_auto_test.py COM8 --tx-debug-port COM11 --rx-debug-port COM13 --suite square`
3. `py -3 .\stress_test.py COM8 --tx-debug-port COM11 --rx-debug-port COM13 --suite repeat --rounds 20 --cycles 10 --report-json result.json`
4. 最终留档：`py -3 .\stress_test.py COM8 --tx-debug-port COM11 --rx-debug-port COM13 --suite repeat --rounds 20 --cycles 50 --report-json result.json`
