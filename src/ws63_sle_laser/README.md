# WS63 SLE Laser Marker (Bidirectional Bridge Design)

极简设计：发射板作为**双向无线串口桥**，接收板复用ws63_laser_single逻辑。

## 架构

```
┌─────────────────────┐                    ┌─────────────────────┐
│   发射板 (无线串口桥) │      SLE无线       │   接收板             │
│                     │                    │                     │
│  UART RX ──────────►│──── G-code字符串 ──►│──► G-code解析       │
│  (PC发来的G-code)   │                    │    运动控制          │
│                     │                    │    DAC/PWM输出       │
│  UART TX ◄──────────│◄── ok/error/status ◄│◄── 响应返回         │
│  (返回给PC)         │                    │                     │
└─────────────────────┘                    └─────────────────────┘
```

## 关键设计点

### 1. 双向透传（必须！）

```
PC → UART → 发射板 → SLE → 接收板 → 执行
PC ← UART ← 发射板 ← SLE ← 接收板 ← ok/error
```

没有回传，LaserGRBL会卡在"Waiting"，因为收不到`ok`。

### 2. 分包处理

SLE是按包收发的，不能假设一包就是一行G-code：
- 接收端逐字节塞进G-code行缓冲
- 遇到`\n`或`\r`才解析一行

### 3. 实时命令

GRBL协议中的实时命令不能等到换行再处理：
- `?` 状态查询
- `!` 暂停
- `~` 恢复
- `Ctrl-X` 软复位

### 4. 流控

保留GRBL风格的`ok`节奏：
- 接收一行 → 缓冲成功 → 返回ok
- 发射板不自己伪造ok，必须等接收板真正返回

## 文件结构

```
ws63_sle_laser/
├── CMakeLists.txt
├── Kconfig
├── README.md
├── transmitter/              # 发射板（双向无线串口桥）
│   ├── CMakeLists.txt
│   ├── main.c               # UART↔SLE双向透传
│   └── sle_passthrough.h/c  # SLE透传模块
└── receiver/                 # 接收板（复用ws63_laser_single）
    ├── CMakeLists.txt
    ├── main.c               # G-code处理 + SLE回复
    ├── sle_receiver.h/c     # SLE接收+回复模块
    ├── config.h             # 配置
    ├── gcode_parser.h/c     # 复用自ws63_laser_single
    ├── gcode_processor.h/c
    ├── motion_executor.h/c
    ├── dac8562.h/c
    └── laser_ctrl.h/c
```

## 数据流

### 发射板

```c
// UART RX → SLE TX
void process_char(uint8_t ch) {
    if (ch == '\n' || ch == '\r') {
        sle_passthrough_send_line(g_line, g_line_pos);
    }
}

// SLE RX → UART TX
void on_sle_response(const uint8_t *data, uint16_t length) {
    uapi_uart_write(UART_BUS, data, length, 0);  // 返回给PC
}
```

### 接收板

```c
// SLE RX → G-code处理
void sle_gcode_line_received(const char *line, uint16_t len) {
    process_line(line, len);  // 和ws63_laser_single一样
}

// 响应 → SLE TX
void send_response(const char *str) {
    uapi_uart_write(UART_BUS, str, len, 0);  // 本地调试
    sle_receiver_send_response(str, len);     // 返回给发射板
}
```

## 使用方法

### 1. 编译

```bash
python3 build.py -c ws63-liteos-app menuconfig
# Application -> Enable WS63 SLE Laser Marker -> 选择角色

python3 build.py -c ws63-liteos-app
```

### 2. 烧录

- 发射板：烧录TRANSMITTER固件，连接电脑
- 接收板：烧录RECEIVER固件，连接振镜/激光

### 3. 测试步骤

#### 第一步：裸透传测试

用串口助手发：
```
G1 X10 Y10 F1000
M3 S200
M5
?
$$
Ctrl-X
```

验证：
- 接收板收到G-code并执行
- ok/error/status返回到PC

#### 第二步：接入LaserGRBL

先用低频率、短G-code文件测试：
- 是否掉线
- 是否卡在Waiting
- ok返回是否正常
- ?状态查询是否正常
- Ctrl-X软复位是否正常

## 设计优势

1. **发射板极简**：只做透传，不解析G-code
2. **接收板复用**：90%代码来自ws63_laser_single
3. **双向通信**：ok/error/status正确返回
4. **流控正确**：保留GRBL的ok节奏
5. **实时命令**：支持?/!/~/Ctrl-X
