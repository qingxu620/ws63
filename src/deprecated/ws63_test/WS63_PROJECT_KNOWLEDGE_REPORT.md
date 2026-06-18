# WS63 工程知识点全景报告

更新时间：`2026-04-20`  
适用目录：`ws63_test/`

这份报告面向两个目标：

1. 帮你系统梳理这个工程到底用了哪些知识点
2. 帮你为答辩建立“会做、会讲、会被追问也能答”的知识框架

建议使用方式：

1. 先读第 1 章和第 2 章，建立全局认知
2. 再优先看第 `2.1` 节“知识点-位置-软件方法速查矩阵”
3. 再按第 3 章开始逐条学习知识点
4. 学到一个知识点，就去对应源码文件里对照一次
5. 最后用第 4 章和第 5 章准备答辩表达

---

## 1. 项目一句话定义

这是一个基于 `WS63 + LiteOS` 的双板激光打标控制系统，完整链路为：

`上位机 / AI 设计端 -> 发射板 -> SLE -> 接收板 -> DAC / PWM -> 振镜 / 激光执行`

它不是单一模块 demo，而是一个同时覆盖了：

- 嵌入式固件
- 无线通信
- 实时控制
- 图像处理
- AI 应用
- 上位机 GUI
- 自动化测试
- 工程文档

的综合系统工程。

---

## 2. 整体知识地图

如果从“分层系统”的视角看，这个项目大致用了下面 10 层知识：

1. 产品与系统架构设计
2. 嵌入式基础与 WS63 平台开发
3. RTOS 任务调度与并发同步
4. 通信协议设计与数据完整性保护
5. UART / WiFi / TCP / SLE 多链路通信
6. G-Code 解析与运动命令抽象
7. 运动控制、插补、DAC 和 PWM 输出
8. 安全机制、急停和链路超时保护
9. AI 上位机、图像处理与 G-Code 生成
10. 自动化测试、日志、调试工具和工程化交付

换句话说，你们这个项目不是只用了“某一个知识点”，而是把计算机、嵌入式、网络、控制、AI 和软件工程几块内容连起来了。

---

## 2.1 知识点 - 位置 - 软件方法速查矩阵

这一节的目标是帮你把每个知识点和“代码在哪里、用了什么方法”快速对应起来。  
答辩时如果老师问“你们这个功能是怎么做的”，你可以优先按这一列回答。

| 知识点 | 典型位置 | 采用的软件方法 |
| --- | --- | --- |
| 双板分工架构 | `README.md`、`CODE_ARCHITECTURE.md`、`transmitter/main.c`、`receiver/main.c` | 用“发射板负责协议桥接、接收板负责实时执行”的分层架构实现职责解耦 |
| 模块化工程组织 | `common/`、`transmitter/`、`receiver/`、`ai_studio/`、`tools/` | 按目录和模块边界拆分，采用分层目录组织与共享模块复用 |
| RTOS 多任务调度 | `transmitter/main.c`、`receiver/main.c`、`common/config.h` | 用 LiteOS 多任务模型 + 不同优先级线程实现并发协作 |
| 同步与线程安全 | `receiver/cmd_queue.c`、`receiver/safety_monitor.c` | 用 `mutex + semaphore + volatile` 实现共享状态保护和生产者消费者同步 |
| 编译时功能裁剪 | `Kconfig`、`common/config.h` | 用 Kconfig 条件编译和宏开关实现发射板/接收板、SoftAP/STA 变体构建 |
| 构建系统 | `CMakeLists.txt`、`build.py` | 用 CMake + 构建脚本实现固件编译、清理和产物输出 |
| 自定义二进制协议 | `common/protocol.h` | 用结构体封包的方式定义 `motion_cmd_t`、`status_pkt_t`、`status_full_pkt_t` |
| 数据完整性校验 | `common/crc16.c`、`common/crc16.h` | 用 CRC16 帧校验方法保证无线命令和状态包完整性 |
| 可靠确认机制 | `transmitter/gcode_processor.c`、`transmitter/sle_client.c`、`receiver/sle_server.c` | 用 `seq / ack_seq` 的停等式确认思路保证命令已入队再回 `ok` |
| UART 文本入口 | `transmitter/uart_handler.c` | 用按行缓冲的文本协议处理方法兼容 Grbl 风格串口输入 |
| Grbl 风格兼容 | `transmitter/gcode_processor.c`、`transmitter/uart_handler.c` | 用本地命令分流 + 状态字符串格式化实现 `?/$I/$G/$CAP?` 兼容 |
| WiFi 模式切换 | `Kconfig`、`transmitter/wifi_gcode_server.c`、`ai_studio/main_window.py` | 用 SoftAP/STA 双模式预设 + 场景化切换按钮实现网络工作流切换 |
| TCP 服务端 | `transmitter/wifi_gcode_server.c`、`tools/wifi_client.py` | 用 `listen/accept/recv/send/close` 的 socket 服务端模型实现 WiFi G-Code 入口 |
| lwIP 网络栈接入 | `transmitter/wifi_gcode_server.c` | 用 `lwip/sockets` 与 `netif` 状态判断实现嵌入式 TCP/IP 服务 |
| 单上游主机控制 | `transmitter/wifi_gcode_server.c` | 用单连接串行化策略避免多主机同时污染同一份 G-Code 状态机 |
| SLE 无线桥接 | `transmitter/sle_client.c`、`receiver/sle_server.c`、`common/protocol.h` | 用 Client/Server + 服务发现 + 特征值读写实现无线命令桥接 |
| 无线流控 | `transmitter/sle_client.c`、`common/config.h` | 用 pending 窗口、`queue_free` 回传和节流策略做拥塞控制 |
| 心跳保活 | `transmitter/main.c`、`common/config.h` | 用独立心跳任务 + 发送抑制策略实现链路保活 |
| G-Code 文本解析 | `transmitter/gcode_parser.c` | 用行缓冲、词法扫描、参数查找和数值提取实现文本解析器 |
| G-Code 语义处理 | `transmitter/gcode_processor.c` | 用模态状态机维护 `G90/G91/M3/M5/S/F` 等上下文语义 |
| 命令队列 | `receiver/cmd_queue.c` | 用固定深度环形缓冲区实现命令缓存 |
| 收包与执行解耦 | `receiver/sle_server.c`、`receiver/cmd_queue.c`、`receiver/interpolator.c` | 用生产者消费者模型把“入队确认”和“执行完成”分离 |
| 线性插补 | `receiver/interpolator.c`、`common/config.h` | 用路径离散化 + 步进迭代更新实现直线插补 |
| 坐标边界保护 | `receiver/interpolator.c`、`ai_studio/gcode_generator.py` | 用参数校验 + `clamp` 钳位方法防止越界 |
| 可打断执行 | `receiver/interpolator.c` | 用全局中止标志 + 可中断延时逻辑实现急停打断 |
| DAC 振镜输出 | `receiver/dac8562.c`、`receiver/dac8562.h` | 用 SPI 驱动 DAC8562，把数字坐标映射为模拟电压输出 |
| PWM 激光控制 | `receiver/laser_ctrl.c`、`receiver/laser_ctrl.h` | 用 PWM 占空比映射 + 使能开关实现功率控制 |
| 安全超时保护 | `receiver/safety_monitor.c`、`common/config.h` | 用独立安全监控任务 + 超时确认计数实现停光保护 |
| 急停 | `transmitter/gcode_processor.c`、`transmitter/wifi_gcode_server.c`、`receiver/interpolator.c` | 用统一急停命令 + 优先停光 + 清空队列的方法实现跨链路急停 |
| 桌面 GUI | `ai_studio/main.py`、`ai_studio/main_window.py`、`ai_studio/qt_compat.py` | 用 Qt 窗口、布局、信号槽和分页工作流构建上位机界面 |
| GUI 耗时任务解耦 | `ai_studio/ai_image_generator.py`、`ai_studio/serial_worker.py` | 用 `QThread + signal/slot` 避免 GUI 主线程阻塞 |
| 任务状态机 | `ai_studio/task_manager.py`、`ai_studio/main_window.py` | 用有限状态机管理 `AI生成 -> 轮廓提取 -> G-Code -> 下发 -> 完成/失败` |
| 历史任务留档 | `ai_studio/task_manager.py` | 用任务目录 + JSON 元数据 + 产物文件归档实现可追溯管理 |
| 云端 AI 生图 | `ai_studio/ai_image_generator.py` | 用 REST API `POST + JSON` 调用 Google Imagen，并做超时与异常处理 |
| Prompt 模板化 | `ai_studio/main_window.py`、`ai_studio/ai_image_generator.py` | 用模板提示词 + 固定后缀约束提高出图稳定性和轮廓友好性 |
| base64 图片落盘 | `ai_studio/ai_image_generator.py` | 用 base64 解码 + MIME 后缀推断 + 本地文件保存处理 AI 返回图像 |
| 图像读取兼容 | `ai_studio/image_processing.py` | 用 `np.fromfile + cv2.imdecode` 解决中文路径读取问题 |
| 图像预处理 | `ai_studio/image_processing.py` | 用灰度化 + CLAHE + 高斯模糊 + 膨胀的 OpenCV 流水线增强轮廓 |
| 边缘与轮廓提取 | `ai_studio/image_processing.py` | 用 `Canny + findContours + approxPolyDP + 面积/周长过滤` 提取可雕刻路径 |
| 几何归一化 | `ai_studio/image_processing.py`、`ai_studio/gcode_generator.py` | 用 bounding box、归一化坐标、等比例缩放和居中排版统一图形尺寸 |
| G-Code 生成 | `ai_studio/gcode_generator.py` | 用 `M5 -> G0 -> M3 -> G1 -> M5` 的路径模板生成雕刻指令 |
| 预览路径 | `ai_studio/gcode_generator.py` | 用外接矩形边框路径生成方法实现不开激光的范围预览 |
| 串口/WiFi 统一发送 | `ai_studio/serial_worker.py` | 用传输层抽象 + 统一发送队列兼容串口和 WiFi |
| 逐行发送与 `ok` 同步 | `ai_studio/serial_worker.py` | 用 stop-and-wait 策略实现逐行发送、逐行等待确认 |
| WiFi Python 客户端 | `tools/wifi_client.py` | 用 socket 客户端封装 + 行协议解析实现调试与自动化复用 |
| WiFi 命令行工具 | `tools/wifi_bridge.py` | 用命令行桥接方式把手工命令、文件和 TCP 会话打通 |
| 本地网页控制台 | `tools/wifi_console.py`、`tools/wifi_console.html` | 用 `http.server + JSON API + 轻量前后端分离` 实现调试页面 |
| WiFi 自动化测试 | `tools/wifi_test.py` | 用 smoke/square/repeat/status 测试套件 + JSON 报告做回归验证 |
| UART 自动化测试 | `tools/uart_auto_test.py` | 用脚本化串口发指令、采状态、比结果的方法做回归 |
| 压力测试 | `tools/stress_test.py` | 用 repeat cycle / soak test 的方式统计长时间稳定性 |
| 多层日志与观测性 | 固件日志、`?`、`$WIFI?`、GUI 状态条、历史任务 | 用“日志 + 查询命令 + 状态面板”的组合方式提升可诊断性 |
| 错误可读化 | `ai_studio/task_manager.py` | 用异常映射和中文提示把底层错误转换为用户可理解反馈 |
| 文档化与交接 | `README.md`、`CODE_ARCHITECTURE.md`、`WS63_DEBUG_QUICKSTART.md`、`PROJECT_CAPABILITY_FEASIBILITY_PLAN.md` | 用分层文档体系把使用、调试、架构和规划分开管理 |

---

## 3. 全量知识点拆解

下面按“知识类别 -> 本项目中的体现 -> 对应源码 -> 答辩应该会讲什么”的方式展开。  
如果你想快速回答“这一块是用什么软件方法实现的”，优先回看第 `2.1` 节速查矩阵。

---

## 3.1 系统架构与工程分层

### 3.1.1 双板分工架构

知识点：

- 系统分层设计
- 主控与执行器分离
- 控制板与执行板职责解耦
- 上位机、发射板、接收板三级架构

本项目体现：

- 发射板负责“接收上位机命令、解析 G-Code、通过 SLE 下发”
- 接收板负责“接收命令、排队、插补、驱动 DAC/PWM 执行”
- 上位机负责“设计、任务生成、下发和展示”

源码位置：

- `README.md`
- `CODE_ARCHITECTURE.md`
- `transmitter/main.c`
- `receiver/main.c`

答辩应该会讲：

- 为什么要双板而不是单板
- 为什么把“协议解析”和“实时执行”拆开
- 这样拆分后有什么好处：职责清晰、无线桥接更方便、执行更稳定、扩展更自然

### 3.1.2 模块化工程组织

知识点：

- 按职责拆目录
- 公共模块复用
- 固件层与工具层分离
- GUI 上位机与固件解耦

本项目体现：

- `common/` 放共享协议和配置
- `transmitter/` 放发射板逻辑
- `receiver/` 放接收板逻辑
- `ai_studio/` 放 PC 上位机
- `tools/` 放调试和自动化测试工具

答辩应该会讲：

- 这是“可维护工程”，不是脚本堆在一起
- 新同学接手时可以快速定位问题层

---

## 3.2 嵌入式平台与 LiteOS / OpenHarmony 基础

### 3.2.1 嵌入式开发基础

知识点：

- MCU/SoC 固件开发
- 裸机与 RTOS 的区别
- 外设初始化
- GPIO、SPI、UART、PWM 基础

本项目体现：

- 接收板初始化 `SPI0`、`PWM2`
- 发射板初始化 `UART1`
- 两块板都基于 WS63 平台运行

源码位置：

- `common/config.h`
- `receiver/main.c`
- `transmitter/main.c`

### 3.2.2 RTOS 任务模型

知识点：

- 线程 / 任务
- 任务优先级
- 周期任务
- 阻塞等待与异步调度
- 让出 CPU 的意义

本项目体现：

- 发射板有 UART 任务、WiFi 任务、SLE 初始化任务、心跳任务
- 接收板有插补任务、安全任务、SLE 初始化任务
- 插补任务优先级高于普通通信任务

源码位置：

- `transmitter/main.c`
- `receiver/main.c`
- `common/config.h`

答辩应该会讲：

- 为什么插补和安全优先级高
- 为什么 WiFi 任务优先级低于实时控制链路
- 为什么长轨迹执行时还要周期性 `yield`

### 3.2.3 同步原语

知识点：

- `mutex`
- `semaphore`
- 生产者消费者模型
- 线程安全共享资源

本项目体现：

- 接收板命令队列使用 `mutex + semaphore`
- 多线程共享状态时用 `volatile`

源码位置：

- `receiver/cmd_queue.c`
- `receiver/safety_monitor.c`

答辩应该会讲：

- 为什么队列不能直接裸数组读写
- 为什么需要互斥锁保护 head/tail
- 为什么要信号量唤醒消费者

---

## 3.3 构建系统与可配置工程

### 3.3.1 Kconfig 配置系统

知识点：

- 编译时特性开关
- 发射板 / 接收板角色选择
- SoftAP / STA 模式选择
- 固件变体构建

本项目体现：

- `LASER_MARKER_RECEIVER`
- `LASER_MARKER_TRANSMITTER`
- `LASER_WIFI_MODE_SOFTAP`
- `LASER_WIFI_MODE_STA`

源码位置：

- `Kconfig`
- `common/config.h`

### 3.3.2 CMake 与构建产物

知识点：

- 嵌入式工程构建系统
- 固件包和 ELF 的区别
- 清理编译与增量编译

本项目体现：

- `CMakeLists.txt`
- `build.py menuconfig`
- `build.py ws63-liteos-app`

答辩应该会讲：

- 系统支持按角色生成不同固件
- 配置和代码不是硬编码耦合，而是通过构建系统打通

---

## 3.4 通信协议设计

### 3.4.1 自定义二进制协议

知识点：

- 命令包设计
- 状态包设计
- 结构体打包
- 字段语义设计
- 命令类型、状态码、错误码

本项目体现：

- `motion_cmd_t`
- `status_pkt_t`
- `status_full_pkt_t`

源码位置：

- `common/protocol.h`

你必须会讲的字段：

- `cmd`
- `flags`
- `seq`
- `target_x / target_y`
- `feed_rate`
- `laser_pwr`
- `ack_seq`
- `queue_free`
- `status`
- `error_code`

答辩应该会讲：

- 为什么不直接发文本到接收板，而要抽象成结构化命令包
- 为什么状态包里要有 `ack_seq` 和 `queue_free`

### 3.4.2 CRC16 校验

知识点：

- 数据完整性校验
- 无线链路误码防护
- 收发一致性校验

本项目体现：

- 每条业务命令带 `crc16`
- 接收板收包时先校验再入队

源码位置：

- `common/crc16.c`
- `common/crc16.h`
- `common/protocol.h`

答辩应该会讲：

- 为什么无线链路不能只靠“收到就算成功”
- 为什么 CRC 是必要的基础可靠性措施

### 3.4.3 序号与 ACK 机制

知识点：

- `seq / ack`
- 命令确认
- 重发
- 去重和可靠投递思路

本项目体现：

- 发射板为每条业务命令分配 `seq`
- 接收板状态回包中带 `ack_seq`
- 发射板等待 ACK 后再向上位机返回 `ok`

源码位置：

- `transmitter/gcode_processor.c`
- `transmitter/wifi_gcode_server.c`
- `transmitter/sle_client.c`
- `receiver/sle_server.c`

答辩应该会讲：

- 为什么发射板不是“收到串口一行就立即 ok”
- 为什么要尽量等接收板入队确认

---

## 3.5 串口通信知识

### 3.5.1 UART 基础

知识点：

- UART 收发
- 波特率
- 文本命令按行处理
- 业务串口与调试串口分离

本项目体现：

- 发射板 `UART1` 用于上位机业务通信
- `UART0` 常用于调试日志

源码位置：

- `common/config.h`
- `transmitter/uart_handler.c`

### 3.5.2 Grbl 风格文本协议兼容

知识点：

- 文本 G-Code 协议
- `ok / error / status`
- `$I`、`$G`、`?` 等控制命令

本项目体现：

- 发射板兼容 Grbl 风格交互
- `?` 返回 `<Idle|MPos:...>`
- `$CAP?` 返回能力画像

源码位置：

- `transmitter/gcode_processor.c`
- `transmitter/uart_handler.c`

答辩应该会讲：

- 为什么选择兼容 Grbl 风格
- 这样可以降低上位机接入门槛

---

## 3.6 WiFi、SoftAP、STA、TCP、lwIP

### 3.6.1 WiFi 模式设计

知识点：

- SoftAP 热点模式
- STA 入网模式
- 场景化网络设计
- 现场直连与联网工作流的区别

本项目体现：

- `SoftAP` 适合比赛现场直连
- `STA` 适合 AI 生图和局域网联动

源码位置：

- `Kconfig`
- `common/config.h`
- `transmitter/wifi_gcode_server.c`
- `ai_studio/main_window.py`

### 3.6.2 TCP Socket 编程

知识点：

- `listen / accept / recv / send / close`
- backlog
- 会话管理
- 连接重试
- 服务端欢迎信息

本项目体现：

- 发射板开启 TCP 服务，默认 `5000`
- 建连后发送欢迎语和使用约束
- WiFi 文本入口与 UART 文本入口语义一致

源码位置：

- `transmitter/wifi_gcode_server.c`
- `tools/wifi_client.py`

答辩应该会讲：

- WiFi TCP 不是另起一套运动协议，而是复用已有 G-Code 入口
- 这样网页端、上位机、Python 工具都能复用

### 3.6.3 lwIP 与嵌入式网络栈

知识点：

- 嵌入式 TCP/IP 协议栈
- `lwip/sockets`
- 网络接口状态判断
- IP 获取与网络 ready 判定

本项目体现：

- 发射板判断 `netif`
- 区分 SoftAP 与 STA 的网络就绪条件

源码位置：

- `transmitter/wifi_gcode_server.c`

### 3.6.4 单上游主机约束

知识点：

- 单会话仲裁
- 避免多个上游同时污染同一份 G-Code 状态机

本项目体现：

- 板端明确限制一次只允许一个上游主机有效控制
- 欢迎语中直接提示 `One upstream host at a time`

答辩应该会讲：

- 这是当前工程阶段的稳定性权衡
- 后续若要做多主机并发，需要增加入口仲裁层

---

## 3.7 星闪 SLE 无线链路

### 3.7.1 SLE Client / Server 架构

知识点：

- 设备发现
- 建连
- 服务发现
- 特征值读写
- 状态通知

本项目体现：

- 发射板是 `SLE Client`
- 接收板是 `SLE Server`
- 发射板主动扫描 `LaserRX`
- 建立链路后通过 SSAP 特征值收发命令与状态

源码位置：

- `transmitter/sle_client.c`
- `receiver/sle_server.c`
- `common/protocol.h`

### 3.7.2 无线链路流控

知识点：

- 在途写请求窗口
- 业务流量与心跳流量竞争
- 队列剩余空间回传
- 拥塞控制

本项目体现：

- `SLE_TX_MAX_PENDING_WRITES`
- `SLE_TX_HEARTBEAT_MAX_PENDING`
- `SLE_TX_BUSINESS_MAX_PENDING`
- `queue_free` 决定发射节流

源码位置：

- `common/config.h`
- `transmitter/main.c`
- `transmitter/sle_client.c`
- `transmitter/wifi_gcode_server.c`

答辩应该会讲：

- 为什么心跳和业务命令不能不加区分地共抢同一个窗口
- 为什么接收板要把 `queue_free` 回传给发射板

### 3.7.3 心跳保活

知识点：

- 保活机制
- 周期心跳
- 链路活跃检测
- 心跳抑制策略

本项目体现：

- 发射板独立心跳任务发送 `CMD_HEARTBEAT`
- 业务刚发送后会短时间抑制心跳，避免抢占业务窗口

源码位置：

- `transmitter/main.c`
- `common/config.h`

答辩应该会讲：

- 为什么心跳必须独立成任务
- 为什么“保活”和“实时业务”之间要做权衡

---

## 3.8 G-Code 解析与控制语义

### 3.8.1 文本解析器

知识点：

- 字符流处理
- 行缓冲
- 大小写归一化
- 注释去除
- 参数识别
- 数值提取

本项目体现：

- `gcode_add_char`
- `gcode_parse`
- `gcode_has_word`
- `gcode_get_value`

源码位置：

- `transmitter/gcode_parser.c`

### 3.8.2 G-Code 语义处理

知识点：

- `G0 / G1`
- `G90 / G91`
- `G92`
- `M3 / M4 / M5`
- `S`
- `F`
- 模态状态维护

本项目体现：

- 发射板维护本地控制上下文：
  - 当前速度
  - 当前功率
  - 当前激光开关
  - 绝对/相对模式
  - 当前预测位置

源码位置：

- `transmitter/gcode_processor.c`

答辩应该会讲：

- 文本 G-Code 不是直接传给接收板，而是先转成标准化的 `motion_cmd_t`
- 发射板是“解释器 + 协议转换器”

### 3.8.3 本地命令与能力画像

知识点：

- 本地管理命令
- 固件版本查询
- 模态查询
- 能力声明

本项目体现：

- `$I`
- `$G`
- `$CAP?`
- `?`

答辩应该会讲：

- 项目不仅能执行动作，还能自描述自身能力
- 这对调试、展示和网页控制台都很重要

---

## 3.9 命令队列与生产者消费者模型

### 3.9.1 环形队列

知识点：

- Ring Buffer
- head / tail
- 队列满判定
- 队列空判定

本项目体现：

- 接收板命令缓存使用固定深度环形队列

源码位置：

- `receiver/cmd_queue.c`

### 3.9.2 队列解耦

知识点：

- 收包线程与执行线程解耦
- 收到命令不等于立即执行
- 入队确认与执行完成的区别

本项目体现：

- 接收板 SLE 收包后先入队
- 插补线程异步消费

答辩应该会讲：

- `ack_seq` 表示“已入队”，不是“已执行完”
- 这是工业控制里很重要的时序语义

---

## 3.10 运动控制与插补算法

### 3.10.1 坐标系与工作区

知识点：

- 工作区定义
- 物理单位 mm
- X/Y 坐标范围
- 第一象限正坐标工作流

本项目体现：

- 工作区默认 `100mm x 100mm`
- 上位机与固件统一正坐标语义

源码位置：

- `common/config.h`
- `ai_studio/gcode_generator.py`

### 3.10.2 坐标钳位与边界保护

知识点：

- 越界保护
- 目标点钳位
- 输入参数合法性校验

本项目体现：

- 接收板对坐标进行 `clamp`
- 上位机在 G-Code 生成前就校验尺寸上限

源码位置：

- `receiver/interpolator.c`
- `ai_studio/gcode_generator.py`

### 3.10.3 线性插补

知识点：

- 直线插补
- 路径离散化
- 速度换算
- 步长控制
- 微秒级与毫秒级混合延时

本项目体现：

- `STEP_NUM`
- 根据距离计算步数
- `feed_rate mm/min -> mm/s`
- 每步更新 `DAC`

源码位置：

- `receiver/interpolator.c`
- `common/config.h`

答辩应该会讲：

- 为什么不能从起点直接“跳”到终点
- 为什么需要按固定步长平滑拆分

### 3.10.4 实时中断与可打断执行

知识点：

- 运动中止
- 急停打断
- 延时过程可中断

本项目体现：

- `g_abort_requested`
- 延时函数可被急停打断
- 急停后清队列、停光、退出当前运动

源码位置：

- `receiver/interpolator.c`

---

## 3.11 模拟输出与硬件执行

### 3.11.1 DAC 驱动

知识点：

- SPI 总线
- DAC 芯片驱动
- 数字坐标到模拟电压
- X/Y 双通道输出

本项目体现：

- `DAC8562`
- 通过 SPI 输出 X/Y 值到振镜

源码位置：

- `receiver/dac8562.c`
- `receiver/dac8562.h`

### 3.11.2 PWM 激光控制

知识点：

- PWM 原理
- 占空比与功率映射
- 激光使能控制

本项目体现：

- `laser_set_power`
- `laser_enable`

源码位置：

- `receiver/laser_ctrl.c`
- `receiver/laser_ctrl.h`

答辩应该会讲：

- X/Y 是模拟量控制
- 激光功率是 PWM 控制
- 运动和能量输出是两条不同的执行通道

---

## 3.12 安全机制、急停与看门狗思想

### 3.12.1 链路安全超时

知识点：

- 通信中断检测
- 业务活跃与空闲场景区分
- 连续确认避免误判

本项目体现：

- `SAFETY_SLE_TIMEOUT_MS`
- `SAFETY_SLE_TIMEOUT_ACTIVE_MS`
- `SAFETY_TIMEOUT_CONFIRM_COUNT`

源码位置：

- `common/config.h`
- `receiver/safety_monitor.c`

### 3.12.2 急停

知识点：

- 紧急停止语义
- 跨链路急停命令统一
- 停光优先级高于运动完成

本项目体现：

- `!`
- `$STOP`
- `M112`
- `CMD_EMERGENCY_STOP`

源码位置：

- `transmitter/gcode_processor.c`
- `transmitter/wifi_gcode_server.c`
- `receiver/interpolator.c`

### 3.12.3 安全默认值

知识点：

- 上电默认安全
- 激光默认关闭
- 故障后回到安全态

本项目体现：

- 安全模块初始化时关闭激光
- 超时后关闭激光并清空队列

源码位置：

- `receiver/safety_monitor.c`

答辩应该会讲：

- 这是工业设备安全设计思路，而不是“功能能跑就算完”

---

## 3.13 Python 上位机与桌面 GUI

### 3.13.1 GUI 框架

知识点：

- `PySide6 / PyQt5`
- 窗口布局
- 事件驱动
- 信号槽机制
- 多页面工作流 UI

本项目体现：

- `main.py`
- `main_window.py`
- `qt_compat.py`

### 3.13.2 线程与界面解耦

知识点：

- GUI 主线程不能阻塞
- 子线程执行耗时任务
- 线程结果通过信号回传

本项目体现：

- AI 生图在单独线程中执行
- 发送线程独立于 GUI

源码位置：

- `ai_studio/ai_image_generator.py`
- `ai_studio/serial_worker.py`
- `ai_studio/main_window.py`

### 3.13.3 状态机与任务管理

知识点：

- 任务生命周期
- 状态机设计
- 任务留档
- 历史记录
- 错误可读化

本项目体现：

- `TaskState`
- `TaskRecord`
- `TaskManager`
- 历史任务列表
- 任务产物目录

源码位置：

- `ai_studio/task_manager.py`
- `ai_studio/main_window.py`

答辩应该会讲：

- 上位机不是“点一个按钮然后盲发”
- 它有明确的任务状态流和产物留档能力

---

## 3.14 云端 AI 生图与 API 调用

### 3.14.1 云端 AI 服务调用

知识点：

- REST API
- HTTP POST
- JSON 请求体
- 超时处理
- 返回值解析

本项目体现：

- 调用 Google Imagen 4 Fast

源码位置：

- `ai_studio/ai_image_generator.py`

### 3.14.2 Prompt Engineering

知识点：

- 提示词模板
- 输出风格约束
- 展示效果与雕刻效果平衡
- 生成稳定性控制

本项目体现：

- 已固化 3 套比赛模板
- 有后缀提示词增强轮廓提取友好性

源码位置：

- `ai_studio/main_window.py`
- `ai_studio/ai_image_generator.py`

答辩应该会讲：

- 不是随便一句 prompt 就直接上设备
- 我们对提示词做了面向雕刻任务的约束设计

### 3.14.3 base64 图片处理

知识点：

- AI 接口返回图片数据编码
- base64 解码
- 图片落盘
- MIME 推断

本项目体现：

- AI 返回图像后保存到本地供后续图像处理

源码位置：

- `ai_studio/ai_image_generator.py`

---

## 3.15 图像处理与 OpenCV

### 3.15.1 图像读取与编码兼容

知识点：

- 中文路径兼容
- `cv2.imread` 的限制
- `np.fromfile + cv2.imdecode`

本项目体现：

- Windows 中文路径图片读取问题已修复

源码位置：

- `ai_studio/image_processing.py`

### 3.15.2 图像预处理

知识点：

- 灰度化
- CLAHE
- Gaussian Blur
- 形态学操作
- 噪声抑制

本项目体现：

- 先增强局部对比，再平滑，再边缘提取

源码位置：

- `ai_studio/image_processing.py`

### 3.15.3 边缘与轮廓提取

知识点：

- Canny 边缘检测
- `findContours`
- 面积与周长过滤
- 多边形逼近 `approxPolyDP`

本项目体现：

- 从 AI 图和本地图中提取可雕刻路径

源码位置：

- `ai_studio/image_processing.py`

### 3.15.4 几何归一化与居中排版

知识点：

- 坐标归一化
- 图像坐标到雕刻坐标转换
- bounding box
- 等比例缩放
- 居中摆放

本项目体现：

- 把任意图像轮廓统一映射到 0~1 空间
- 再适配到用户设定的物理尺寸

源码位置：

- `ai_studio/image_processing.py`
- `ai_studio/gcode_generator.py`

答辩应该会讲：

- 为什么 AI 图片不能直接转 G-Code
- 为什么要先得到规范化轮廓

---

## 3.16 G-Code 生成与路径规划基础

### 3.16.1 几何映射到物理尺寸

知识点：

- 归一化路径到 mm 尺寸映射
- 物理宽高比保持
- 工作区二次 fit

本项目体现：

- 用户设置 50x50 或 80x50 时，图案不会被拉伸变形

源码位置：

- `ai_studio/gcode_generator.py`

### 3.16.2 G-Code 生成策略

知识点：

- 路径起点移动
- 激光开关时机
- 雕刻段与空走段区分
- 收尾回零

本项目体现：

- `M5 -> G0 -> M3 -> G1 ... -> M5`
- 最终回到 `X0 Y0`

源码位置：

- `ai_studio/gcode_generator.py`

### 3.16.3 预览路径

知识点：

- 边框预览
- 激光关闭状态下的范围确认

本项目体现：

- 生成预览 G-Code 用于现场边界确认

源码位置：

- `ai_studio/gcode_generator.py`

答辩应该会讲：

- 我们不是只会“打”，还支持先预览范围，降低误打风险

---

## 3.17 串口 / WiFi 融合发送线程

### 3.17.1 统一发送抽象

知识点：

- 传输层抽象
- 串口与 WiFi 统一接口
- 队列式任务发送

本项目体现：

- `serial_worker.py` 同时支持串口和 WiFi

源码位置：

- `ai_studio/serial_worker.py`

### 3.17.2 逐行发送与等待 ok

知识点：

- 应答式通信
- 上下位机节拍同步
- 发送进度回传

本项目体现：

- 一次只发一行
- 收到 `ok` 再发下一行

答辩应该会讲：

- 这不是极限速度方案，但它是当前比赛阶段最稳妥的方案

### 3.17.3 急停融合

知识点：

- 串口急停和 WiFi 急停策略差异

本项目体现：

- 串口模式下发送 `M5` 与 `!`
- WiFi 模式下发送 `!`

源码位置：

- `ai_studio/serial_worker.py`

---

## 3.18 WiFi 工具链与网页控制台

### 3.18.1 Python 传输层封装

知识点：

- Socket 客户端封装
- 行协议读取
- 欢迎信息解析
- 状态行解析
- WiFi 状态解析

本项目体现：

- `WifiGcodeClient`

源码位置：

- `tools/wifi_client.py`

### 3.18.2 命令行桥接工具

知识点：

- 命令行交互工具
- 文件到 G-Code 发送
- TCP 手工联调

本项目体现：

- `wifi_bridge.py`

### 3.18.3 Web 控制台

知识点：

- 本地网页控制台
- 前后端分离的轻量实现
- 状态灯与日志面板

本项目体现：

- `wifi_console.py`
- `wifi_console.html`

### 3.18.4 WiFi 自动化测试

知识点：

- 建连验证
- 状态验证
- smoke / square / repeat / status 套件
- JSON 报告输出

本项目体现：

- `wifi_test.py`

答辩应该会讲：

- WiFi 不是“能连一次就算成功”
- 我们有自动化脚本和网页工具来验证和展示它

---

## 3.19 自动化测试与回归验证

### 3.19.1 UART 自动化测试

知识点：

- 串口自动回归
- 用例设计
- 状态采样
- 失败留档

本项目体现：

- `uart_auto_test.py`

### 3.19.2 压力测试

知识点：

- soak test
- repeat cycles
- 稳定性统计

本项目体现：

- `stress_test.py`

### 3.19.3 JSON 报告与测试工程化

知识点：

- 测试报告结构化保存
- 机器可读结果输出
- 留档与追溯

本项目体现：

- UART 与 WiFi 测试都支持 `report-json`

答辩应该会讲：

- 我们不是靠“现场运气好”证明系统可用
- 我们有回归测试和留档机制

---

## 3.20 日志、观测性与调试思维

### 3.20.1 运行日志

知识点：

- 初始化日志
- 状态日志
- 错误日志
- 周期统计日志

本项目体现：

- 发射板心跳统计日志
- WiFi 状态日志
- 安全超时日志
- 上位机用户日志

### 3.20.2 调试入口设计

知识点：

- `?`
- `$WIFI?`
- `$CAP?`
- GUI 状态条
- 历史任务

本项目体现：

- 固件、工具、GUI 三层都有自己的调试入口

答辩应该会讲：

- 一个好工程不只是能跑，还得能诊断

---

## 3.21 错误处理与鲁棒性

### 3.21.1 参数校验

知识点：

- 输入合法性验证
- 坐标范围检查
- 功率范围检查
- 空命令过滤

本项目体现：

- 固件侧和上位机侧都有边界检查

### 3.21.2 超时与重试

知识点：

- ACK 超时
- 连接重试
- WiFi 会话切换等待
- SLE busy 重试

本项目体现：

- `CMD_ACK_TIMEOUT_MS`
- `CMD_RETRY_MAX`
- WiFi 客户端重连重试
- SLE busy 时短等待再试

### 3.21.3 错误可读化

知识点：

- 技术错误转用户可读提示
- GUI 错误映射

本项目体现：

- `TaskManager.get_readable_error`

源码位置：

- `ai_studio/task_manager.py`

---

## 3.22 人机交互与比赛展示设计

### 3.22.1 模板化操作流程

知识点：

- 降低使用门槛
- 把复杂流程做成一步步引导

本项目体现：

- 三步骤 GUI
- 比赛模板
- 一键比赛流程

### 3.22.2 展示化表达

知识点：

- 技术能力产品化
- 面向评委的状态可视化
- 历史任务与成果复用

本项目体现：

- 顶部任务状态条
- 历史任务卡片
- 产物目录留档

答辩应该会讲：

- 我们不仅实现了功能，也把流程做成了可展示、可复用、可追溯的系统

---

## 3.23 软件工程与文档化能力

### 3.23.1 文档体系

知识点：

- README
- 架构文档
- 调试手册
- 验收清单
- 能力规划文档

本项目体现：

- `README.md`
- `CODE_ARCHITECTURE.md`
- `WS63_DEBUG_QUICKSTART.md`
- `PROJECT_CAPABILITY_FEASIBILITY_PLAN.md`

### 3.23.2 交接与维护

知识点：

- 新人可接手性
- 模块边界清晰
- 调试路径清晰

答辩应该会讲：

- 这不是一次性参赛代码，而是具备交接和扩展能力的工程基础

---

## 4. 你答辩时必须掌握的重点知识

如果时间有限，下面这些内容你必须会讲。

### 第一优先级

1. 为什么采用双板架构
2. 发射板和接收板各自做什么
3. 上位机命令如何变成板端运动
4. SLE 在这里起什么作用
5. DAC 和 PWM 分别控制什么
6. 为什么要有安全超时与急停
7. AI 图片为什么要经过 OpenCV 和 G-Code 生成
8. 为什么支持 UART、WiFi SoftAP、WiFi STA 三种接入路径

### 第二优先级

1. `motion_cmd_t` 和 `status_full_pkt_t` 的作用
2. `seq / ack / queue_free` 的意义
3. 为什么要做环形队列
4. 为什么插补任务优先级高
5. 为什么 WiFi 和 UART 当前不建议并发控制
6. 为什么上位机采用逐行发送并等待 `ok`

### 第三优先级

1. CLAHE、Canny、轮廓提取具体做了什么
2. Prompt 模板为什么重要
3. 历史任务和产物留档的价值
4. 自动化测试脚本怎么证明系统稳定

---

## 5. 答辩高频问题清单

下面这些问题，老师和评委很可能会问。

### 架构类

1. 为什么不用单板实现全部功能？
2. 为什么接收板不直接解析 G-Code？
3. 为什么选择 SLE，而不是全程串口？
4. 你们的系统和普通激光雕刻控制器相比，有什么不同？

### 通信类

1. 串口、WiFi、SLE 三条链路分别做什么？
2. 为什么 WiFi 只接入到发射板？
3. 你们如何保证无线下发的可靠性？
4. 为什么需要 CRC？
5. `ack_seq` 为什么重要？

### 控制类

1. 插补是什么？
2. DAC 和 PWM 各自负责什么？
3. 为什么要做坐标钳位？
4. 为什么要有命令队列？
5. 为什么运行中会有 `Idle / Run` 状态？

### 安全类

1. 如果无线断了，激光会不会继续烧？
2. 急停是怎么实现的？
3. 为什么你们的安全设计是可信的？

### AI 类

1. AI 生图和激光雕刻之间怎么衔接？
2. 为什么不是把生成图直接发给设备？
3. 为什么要做轮廓提取？
4. 为什么要做 Prompt 模板而不是让用户随便输？

### 工程类

1. 你们如何验证系统稳定？
2. 你们有哪些自动化测试？
3. 如果交给别人维护，怎么快速接手？

---

## 6. 推荐学习顺序

建议不要平均发力，而是按下面顺序学。

### 第 1 阶段：先会讲系统

先读：

- `README.md`
- `CODE_ARCHITECTURE.md`

目标：

- 能画出系统框图
- 能讲清楚数据流向

### 第 2 阶段：再会讲板端主链路

重点看：

- `common/protocol.h`
- `common/config.h`
- `transmitter/main.c`
- `transmitter/gcode_processor.c`
- `receiver/sle_server.c`
- `receiver/interpolator.c`
- `receiver/safety_monitor.c`

目标：

- 能讲清楚一条命令如何被执行

### 第 3 阶段：再会讲 WiFi 与工具链

重点看：

- `transmitter/wifi_gcode_server.c`
- `tools/wifi_client.py`
- `tools/wifi_test.py`
- `tools/wifi_console.py`

目标：

- 能讲清楚为什么既支持 SoftAP 又支持 STA
- 能讲清楚如何做自动化验证

### 第 4 阶段：最后补 AI 上位机

重点看：

- `ai_studio/main_window.py`
- `ai_studio/task_manager.py`
- `ai_studio/ai_image_generator.py`
- `ai_studio/image_processing.py`
- `ai_studio/gcode_generator.py`

目标：

- 能讲清楚 AI 到真机执行的闭环

---

## 7. 一句话答辩模板

你可以把整个项目概括成下面这段话：

“我们实现的是一个基于 WS63 的双板式激光打标控制系统。上位机可以通过串口或 WiFi 把 G-Code 或 AI 生成后的轨迹任务发送给发射板，发射板负责协议解析和无线桥接，接收板负责实时插补、DAC 振镜控制和 PWM 激光控制。系统具备 SLE 无线通信、ACK 与流控、安全超时停光、真急停、AI 图像到轮廓到 G-Code 的自动转换能力，并配套了 WiFi 调试工具、自动化测试和任务留档机制，体现了完整的嵌入式控制、物联网通信与 AI 应用融合能力。” 

---

## 8. 结论

这个项目真正涉及的知识点，不是单一的“单片机”或者“AI”。

它至少同时覆盖了：

- 嵌入式系统
- RTOS 并发
- 无线通信
- TCP 网络
- 协议设计
- 队列与流控
- 安全机制
- 运动控制
- 模拟与 PWM 输出
- Python GUI
- 云端 AI API
- OpenCV 图像处理
- G-Code 生成
- 自动化测试
- 工程化文档与交付

如果你把这份报告吃透，答辩时你就不是在“背功能”，而是在讲一个真正完整的系统工程。
