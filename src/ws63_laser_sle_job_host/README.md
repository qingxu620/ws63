# WS63 Laser SLE Job Host

这是 `ws63_laser_sle_job` 的 PC 端调试上位机。

它不是 LaserGRBL，也不是旧的逐行 G-code 串口发送器。它用于验证新的 SLE 结构化任务包链路：

```text
PC 上位机
  -> USB 串口
TX 发射板
  -> SLE 标准任务包
RX 接收板
  -> RAM job cache
  -> 本地 G-code 执行
```

## 功能

- 连接 TX 发射板串口。
- 可选同时监听 TX 发射板调试串口和 RX 接收板调试串口。
- 打开或编辑 G-code 文件。
- 自动计算 G-code 原始字节的 CRC16-CCITT。
- 发送：

```text
@BEGIN <job_id> <total_size> <crc16>
```

- 等待 TX 返回：

```text
@DATA_READY job=<job_id> size=<total_size>
```

- 发送精确 `total_size` 字节 G-code 原始内容。
- 等待：

```text
@JOB_READY job=<job_id> size=<total_size>
```

- 支持：

```text
@EXEC_START <job_id>
@EXEC_STOP
@ABORT
@STATUS
```

## 运行

在 Windows 上安装 Python 3 后，在本目录执行：

```bash
pip install -r requirements.txt
python main.py
```

如果不想污染系统 Python，建议：

```bash
python -m venv .venv
.venv\Scripts\activate
pip install -r requirements.txt
python main.py
```

## 使用顺序

1. 烧录并上电 RX 接收板。
2. 烧录并上电 TX 发射板。
3. 确认 COM26 出现 RX 日志：

```text
[job_rx] advertising as 'sle_job_rx'
[job_rx] SLE connected conn_id=...
```

4. 确认 COM24 出现 TX 日志：

```text
[tx] connected
[tx] ready!
```

5. 打开本工具，选择 TX 发射板对应的命令串口，例如 COM8。
6. 如果要在同一个窗口里看调试日志，选择并打开：

```text
TX日志：COM24
RX日志：COM26
```

7. 点击 `连接`。
8. 先点 `@STATUS`。
9. 点 `上传任务`。
10. 等待日志出现 `JOB_READY`。
11. 点 `执行`，或直接点 `上传并执行`。

注意：Windows 下同一个串口不能被两个软件同时打开。如果本工具监听了 COM24/COM26，
串口助手就不能再打开这两个口。反过来也一样。

## 第一版建议 G-code

先用很短的文件：

```gcode
G90
M3 S50
G1 X10 Y10 F1000
M5
```

第一版 RX RAM 缓存默认 128KB。不要先测试大图，先把协议闭环跑通。

## 日志判断

PC 上位机日志说明 PC 与 TX 串口协议是否正常。

本工具现在可以同时显示三路日志：

```text
TX    PC -> TX 命令串口，例如 COM8
COM24 TX 发射板调试日志
COM26 RX 接收板调试日志
```

真正定位问题仍然要看：

- COM24：TX 发射板日志。
- COM26：RX 接收板日志。

正常上传时，COM24 应看到：

```text
[JOB_TX_FRAME] type=0x01 ...
[JOB_TX_ACK] ack_type=0x01 ... status=0
[JOB_TX_FRAME] type=0x02 ...
[JOB_TX_ACK] ack_type=0x02 ... status=0
[JOB_TX_FRAME] type=0x03 ...
[JOB_TX_ACK] ack_type=0x03 ... status=0
```

COM26 应看到：

```text
[JOB_BEGIN] ... st=0
[JOB_DATA] ... st=0
[JOB_END] ... st=0 state=JOB_READY
[EXEC_START] ... st=0
[JOB_EXEC] start ...
[JOB_EXEC] done ...
```

如果出现 `@NACK` 或 `@ERR`，保存 PC 日志、COM24、COM26 三份日志再分析。

## 注意

- 本工具连接的是 TX 发射板串口，不是 RX 接收板调试串口。
- 本工具不兼容 LaserGRBL 的实时串口协议。
- 本工具不会逐行等待 Grbl `ok`。
- G-code 会作为一个完整任务上传，RX 校验完成后再本地执行。
