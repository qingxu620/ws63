# WS63 Laser Host App

用于调试 WS63 激光打标链路的轻量上位机。

目标不是替代 LaserGRBL 的图像处理和路径规划，而是验证通信链路和固件响应：

- 串口连接 COM8 / 115200
- G-code 按行发送
- 每行等待 `ok` / `error:x`
- `?` 实时状态查询不等待 `ok`
- 超时后自动查询 `?` 和 `$D`
- 记录每行发送时间、响应时间和 RTT
- 支持 `M5`、Ctrl-X 急停

当前版本是串口调试工具。后续 WiFi 方案跑通后，可以在这个独立目录里继续增加 TCP 客户端模式。

## 运行

在 Windows 上安装 Python 后：

```bash
pip install -r requirements.txt
python main.py
```

Linux/WSL 下如果能访问串口，也可以直接运行。

## 推荐测试

先不要用 LaserGRBL，使用本工具发送：

```gcode
$X
G90
M3 S50 F1000
G1 X12 Y5 F1000
G1 X0 Y0 F1000
M5
$D
```

如果这里逐行稳定，再回头分析 LaserGRBL 的连接/轮询/流式发送策略。
