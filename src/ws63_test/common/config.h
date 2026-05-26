/**
 * @file config.h
 * @brief 激光打标机系统全局配置
 */
#ifndef LASER_CONFIG_H
#define LASER_CONFIG_H

/* ================= 振镜/坐标配置 =================
 * 坐标系约定:
 * - 软件坐标原点 (0,0) 对应工作区角点，兼容 LaserGRBL 一类正坐标工作流
 * - DAC 仍然按整幅面利用满量程，避免只用到一小段模拟输出范围
 * - 若后级运放把 0~5V 平移为 -5~+5V，则 0mm 会落在物理负边界
 */
#define DAC_MAX 65535U
#define GALVO_WORK_AREA_X_MM 100.0
#define GALVO_WORK_AREA_Y_MM 100.0
#define GALVO_X_MIN_MM 0.0
#define GALVO_X_MAX_MM (GALVO_WORK_AREA_X_MM)
#define GALVO_Y_MIN_MM 0.0
#define GALVO_Y_MAX_MM (GALVO_WORK_AREA_Y_MM)
#define BEILV_X ((double)DAC_MAX / GALVO_WORK_AREA_X_MM)
#define BEILV_Y ((double)DAC_MAX / GALVO_WORK_AREA_Y_MM)
#define BEILV BEILV_X /* 兼容历史单比例宏 */
#define STEP_NUM 0.1  /* 插补最小距离 (mm) */

/* ================= 速度配置 ================= */
#define DEFAULT_FEED_RATE 10000.0 /* 默认进给速度 mm/min */
#define G0_FEED_RATE 100000.0     /* G0 空走速度 mm/min */
#define LASER_S_MAX 1000.0        /* 激光功率最大值 */

/* ================= 通信配置 ================= */
#define UART_BAUD_RATE 115200
#define CMD_QUEUE_SIZE 32 /* 命令队列深度 */

/* ================= WiFi / TCP 配置 =================
 * 发射板新增一个 WiFi 文本入口，便于后续网页端、电脑端或手机端接入。
 * 它只是“额外入口”，不会替代现有 UART -> G-Code -> SLE 主链路。
 * 由于发射板当前只有一份 G-Code 上下文状态机，建议同一时刻只使用
 * UART 或 WiFi 其中一个上游入口，不要并发向两条入口同时发业务指令。
 */
#if defined(CONFIG_LASER_WIFI_SOFTAP_ENABLE)
#define LASER_WIFI_SOFTAP_ENABLE 1
#else
#define LASER_WIFI_SOFTAP_ENABLE 0
#endif

#if defined(CONFIG_LASER_WIFI_MODE_STA)
#define LASER_WIFI_STA_MODE_ENABLED 1
#else
#define LASER_WIFI_STA_MODE_ENABLED 0
#endif

#define LASER_WIFI_SOFTAP_MODE_ENABLED (!LASER_WIFI_STA_MODE_ENABLED)

#if defined(CONFIG_LASER_WIFI_SOFTAP_SSID)
#define LASER_WIFI_SOFTAP_SSID CONFIG_LASER_WIFI_SOFTAP_SSID
#else
#define LASER_WIFI_SOFTAP_SSID "WS63_LaserTX"
#endif

#if defined(CONFIG_LASER_WIFI_SOFTAP_PSK)
#define LASER_WIFI_SOFTAP_PSK CONFIG_LASER_WIFI_SOFTAP_PSK
#else
#define LASER_WIFI_SOFTAP_PSK "ws63laser"
#endif

#if defined(CONFIG_LASER_WIFI_SOFTAP_CHANNEL)
#define LASER_WIFI_SOFTAP_CHANNEL CONFIG_LASER_WIFI_SOFTAP_CHANNEL
#else
#define LASER_WIFI_SOFTAP_CHANNEL 13
#endif

#if defined(CONFIG_LASER_WIFI_STA_SSID)
#define LASER_WIFI_STA_SSID CONFIG_LASER_WIFI_STA_SSID
#else
#define LASER_WIFI_STA_SSID "YourRouterSSID"
#endif

#if defined(CONFIG_LASER_WIFI_STA_PSK)
#define LASER_WIFI_STA_PSK CONFIG_LASER_WIFI_STA_PSK
#else
#define LASER_WIFI_STA_PSK "routerpass"
#endif

#if defined(CONFIG_LASER_WIFI_TCP_PORT)
#define LASER_WIFI_TCP_PORT CONFIG_LASER_WIFI_TCP_PORT
#else
#define LASER_WIFI_TCP_PORT 5000
#endif

#define LASER_WIFI_SOFTAP_IFNAME "ap0"
#define LASER_WIFI_STA_IFNAME "wlan0"
#define LASER_WIFI_STA_SCAN_TIMEOUT_MS 5000
#define LASER_WIFI_STA_CONNECT_TIMEOUT_MS 5000
#define LASER_WIFI_STA_DHCP_TIMEOUT_MS 15000
#define LASER_WIFI_STA_RETRY_DELAY_MS 2000
/*
 * TCP 会话切换抖动主要出现在“客户端刚断开，下一次连接已发起”的窗口期。
 * 这里把 socket 轮询周期收紧到 200ms，缩短板端感知断链和回到监听/拒绝 busy 的延迟。
 */
#define LASER_WIFI_SOCKET_POLL_TIMEOUT_MS 200

#define LASER_WIFI_SOFTAP_IP_ADDR_1 192
#define LASER_WIFI_SOFTAP_IP_ADDR_2 168
#define LASER_WIFI_SOFTAP_IP_ADDR_3 43
#define LASER_WIFI_SOFTAP_IP_ADDR_4 1

/* ================= SLE 配置 ================= */
#define SLE_LASER_SERVER_NAME "LaserRX"
#define SLE_LASER_CLIENT_NAME "LaserTX"
#define SLE_FOCUS_SERVER_NAME "FocusND"
#define SLE_SAFETY_SERVER_NAME "SafeLED"
/*
 * 发射板侧的多节点框架按“最多 8 个 peer”预留，和官方 sle_one_to_many 例程口径保持一致。
 * 当前业务实际启用的是 LaserRX + FocusND + SafeLED 三个节点，后续仍可继续挂新的 peer。
 */
#define SLE_TX_MAX_PEERS 8

/* ================= 安全配置 ================= */
#define SAFETY_SLE_TIMEOUT_MS 1000        /* 空闲链路 SLE 超时 (ms)，保留 >= 5 个心跳周期余量，避免压测偶发抖动误停光 */
#define SAFETY_SLE_TIMEOUT_ACTIVE_MS 1500 /* 运动/出光期间 SLE 超时 (ms)，配合连续超时确认避免误停光 */
#define SAFETY_SLE_CONNECT_GRACE_MS 2500  /* 连接建立后等待首个业务包的宽限期 (ms) */
#define SAFETY_CHECK_INTERVAL_MS 10       /* 安全检查周期 (ms) */
#define SAFETY_TIMEOUT_CONFIRM_COUNT 3    /* 连续超时确认次数，避免单次调度抖动直接停光 */
#define ACTIVITY_TIMEOUT_MS 200           /* 命令活动超时 → Idle 判定 (ms) */

/* ================= 心跳配置 ================= */
#define HEARTBEAT_INTERVAL_MS 200 /* 发射板心跳周期 (ms) */
/* 心跳仍保持 200ms，但状态通知降到 500ms，降低回传开销并保留足够新的 queue_free/ack 快照 */
#define HEARTBEAT_STATUS_REPORT_INTERVAL_MS 500

/* ===========================================================================
 *  硬件引脚配置  —— 基于 WS63 引脚复用表
 *
 *  接收板 (Receiver):
 *  ┌──────────┬────────┬──────┬──────────┬──────────────────────┐
 *  │ 功能     │ GPIO   │ 引脚 │ PIN_MODE │ 复用功能             │
 *  ├──────────┼────────┼──────┼──────────┼──────────────────────┤
 *  │ SPI CLK  │ GPIO7  │ 31   │ MODE_3   │ SPI0_SCK             │
 *  │ SPI MOSI │ GPIO9  │ 33   │ MODE_3   │ SPI0_OUT             │
 *  │ SPI CS   │ GPIO10 │ 34   │ MODE_0   │ GPIO (手动控制)       │
 *  │ 激光 PWM │ GPIO2  │ 24   │ MODE_1   │ PWM2                 │
 *  └──────────┴────────┴──────┴──────────┴──────────────────────┘
 *
 *  发射板 (Transmitter):
 *  ┌──────────┬────────┬──────┬──────────┬──────────────────────┐
 *  │ 功能     │ GPIO   │ 引脚 │ PIN_MODE │ 复用功能             │
 *  ├──────────┼────────┼──────┼──────────┼──────────────────────┤
 *  │ UART TX  │ GPIO15 │ 9    │ MODE_1   │ UART1_TXD            │
 *  │ UART RX  │ GPIO16 │ 10   │ MODE_1   │ UART1_RXD            │
 *  └──────────┴────────┴──────┴──────────┴──────────────────────┘
 *
 *  ZDT 控制板 (ZDT Controller):
 *  - 使用独立 UART 控制张大头 Emm_V5.0 闭环步进驱动
 *  - 当前感知与对焦节点第一版采用 TTL 串口直连 Emm_V5.0
 *  - Z 轴链路固定复用发送板同款 UART1 / GPIO15-16 接线
 *  - 如果挂 RS485 模块，可选额外一根 GPIO 控制 DE/RE 方向
 * =========================================================================== */

/* --- 接收板: SPI0 (DAC8562) --- */
#define DAC_SPI_BUS 0 /* SPI 总线编号 */
#define DAC_SPI_CLK_PIN 7  /* GPIO7  — Pin 31 — SPI0_SCK  */
#define DAC_SPI_MOSI_PIN 9 /* GPIO9  — Pin 33 — SPI0_OUT  */
#define DAC_CS_PIN 10 /* GPIO10 — Pin 34 — 手动 GPIO */
#define DAC_SPI_PIN_MODE 3 /* PIN_MODE_3 = 复用信号3 (SPI0) */

/* --- 接收板: PWM (激光控制) --- */
#define LASER_PWM_CHANNEL 2  /* PWM2 通道 */
#define LASER_PWM_GROUP_ID 0 /* PWM 分组 ID */
#define LASER_PWM_PIN 2      /* GPIO2  — Pin 24 — PWM2     */
#define LASER_PWM_PIN_MODE 1 /* PIN_MODE_1 = 复用信号1 (PWM) */

/* --- 发射板: UART1 (上位机) --- */
#define LASER_UART_BUS 1 /* UART1 */
#define LASER_UART_TX_PIN 15  /* GPIO15 — Pin 9  — UART1_TXD */
#define LASER_UART_RX_PIN 16  /* GPIO16 — Pin 10 — UART1_RXD */
#define LASER_UART_PIN_MODE 1 /* PIN_MODE_1 = 复用信号1 (UART1) */

/* --- ZDT 控制板: 固定 UART1 (TTL 串口) --- */
#define ZDT_UART_BUS 1 /* UART1 */
#define ZDT_UART_TX_PIN 15 /* GPIO15 — Pin 9  — UART1_TXD */
#define ZDT_UART_RX_PIN 16 /* GPIO16 — Pin 10 — UART1_RXD */
#define ZDT_UART_PIN_MODE 1 /* PIN_MODE_1 = 复用信号1 (UART1) */

#if defined(CONFIG_ZDT_UART_BAUD)
#define ZDT_UART_BAUD_RATE CONFIG_ZDT_UART_BAUD
#else
#define ZDT_UART_BAUD_RATE 115200
#endif

#if defined(CONFIG_ZDT_DEVICE_ADDR)
#define ZDT_DEVICE_ADDR CONFIG_ZDT_DEVICE_ADDR
#else
#define ZDT_DEVICE_ADDR 1
#endif

#if defined(CONFIG_ZDT_RS485_DIR_ENABLE)
#define ZDT_RS485_DIR_ENABLE 1
#else
#define ZDT_RS485_DIR_ENABLE 0
#endif

#if defined(CONFIG_ZDT_RS485_DIR_PIN)
#define ZDT_RS485_DIR_PIN CONFIG_ZDT_RS485_DIR_PIN
#else
#define ZDT_RS485_DIR_PIN 17
#endif

#if defined(CONFIG_ZDT_RS485_DIR_ACTIVE_HIGH)
#define ZDT_RS485_DIR_ACTIVE_HIGH 1
#else
#define ZDT_RS485_DIR_ACTIVE_HIGH 0
#endif

#if defined(CONFIG_ZDT_DEMO_AUTO_RUN)
#define ZDT_DEMO_AUTO_RUN 1
#else
#define ZDT_DEMO_AUTO_RUN 0
#endif

#define ZDT_UART_RX_BUFFER_SIZE 256
#define ZDT_UART_REPLY_TIMEOUT_MS 200
#define ZDT_UART_READ_SLICE_MS 20
#define ZDT_UART_IDLE_GAP_MS 5
#define ZDT_RS485_TURNAROUND_DELAY_US 200
#define ZDT_POLL_INTERVAL_MS 1000
#define ZDT_DEMO_MOVE_SPEED_RPM 300
#define ZDT_DEMO_MOVE_ACCEL_LEVEL 20
#define ZDT_DEMO_MOVE_PULSES 3200U

#if defined(CONFIG_FOCUS_NODE_BOOT_ENABLE_Z)
#define FOCUS_NODE_BOOT_ENABLE_Z 1
#else
#define FOCUS_NODE_BOOT_ENABLE_Z 0
#endif

#if defined(CONFIG_FOCUS_NODE_BOOT_SYNC_ZERO)
#define FOCUS_NODE_BOOT_SYNC_ZERO 1
#else
#define FOCUS_NODE_BOOT_SYNC_ZERO 0
#endif

#if defined(CONFIG_SAFETY_NODE_LED_PIN)
#define SAFETY_NODE_LED_PIN CONFIG_SAFETY_NODE_LED_PIN
#else
#define SAFETY_NODE_LED_PIN 7
#endif

#if defined(CONFIG_SAFETY_NODE_LED_ACTIVE_HIGH)
#define SAFETY_NODE_LED_ACTIVE_HIGH 1
#else
#define SAFETY_NODE_LED_ACTIVE_HIGH 0
#endif

#if defined(CONFIG_SAFETY_NODE_BOOT_LED_ON)
#define SAFETY_NODE_BOOT_LED_ON 1
#else
#define SAFETY_NODE_BOOT_LED_ON 0
#endif

#define SAFETY_NODE_POLL_INTERVAL_MS 1000

/* ================= 任务配置 ================= */
#define TASK_STACK_SIZE_DEFAULT 0x1000
#define TASK_STACK_SIZE_SLE 0x2000
#define TASK_STACK_SIZE_WIFI 0x2000
#define TASK_PRIO_INTERPOLATOR 2 /* OSAL_TASK_PRIORITY_ABOVE_HIGH */
#define TASK_PRIO_SAFETY 2
#define TASK_PRIO_SLE 3 /* OSAL_TASK_PRIORITY_HIGH */
#define TASK_PRIO_UART 3
#define TASK_PRIO_WIFI 24
#define TASK_PRIO_DEFAULT 24

/* ================= 流控配置 ================= */
#define FLOW_CTRL_PAUSE_THRESHOLD 4   /* queue_free < 4 暂停发送 */
#define FLOW_CTRL_RESUME_THRESHOLD 16 /* queue_free > 16 恢复发送 */
#define CMD_RETRY_MAX 3               /* 最大重传次数 */
#define CMD_ACK_TIMEOUT_MS 500        /* ACK 超时时间 */
/* 发射端 SSAP 写请求在途上限，防止链路异常时上下文无限堆积 */
#define SLE_TX_MAX_PENDING_WRITES 8
/*
 * 心跳发送只在总在途写请求较低时才参与竞争，避免心跳把业务命令彻底挤出 SSAP 窗口。
 * 注意：这里限制的是“发送心跳时允许的总 pending 上限”，不是单独统计心跳个数。
 */
#define SLE_TX_HEARTBEAT_MAX_PENDING 2
/* 业务命令可使用更高的在途窗口，优先保证运动命令推进。 */
#define SLE_TX_BUSINESS_MAX_PENDING 6
/* 业务命令撞到 SSAP 写请求窗口上限时，短等待后重试，避免瞬时 BUSY 直接变成 error:2 */
#define SLE_TX_BUSY_RETRY_INTERVAL_MS 5
#define SLE_TX_BUSY_RETRY_TIMEOUT_MS 500
/* 心跳遇到 BUSY 时不必 5ms 疯狂抢窗口，50ms 重试足以兼顾保活和业务优先级。 */
#define SLE_TX_HEARTBEAT_BUSY_RETRY_INTERVAL_MS 50
/* 业务命令刚成功送出后，短时间内不再额外补心跳，避免保活流量与业务命令抢同一窗口。 */
#define SLE_TX_HEARTBEAT_SUPPRESS_AFTER_BUSINESS_MS 250

/* ================= 插补引擎配置 ================= */
#define INTERP_UNLOCK_INTERVAL 200 /* 每 N 步解锁一次调度 */

#endif /* LASER_CONFIG_H */
