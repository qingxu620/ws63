# WS63 现场调试与演示落地手册

配套文档：

- 使用与交付入口：[README.md](./README.md)
- 源码导读与模块职责：[CODE_ARCHITECTURE.md](./CODE_ARCHITECTURE.md)
- AI 上位机说明：[ai_studio/README.md](./ai_studio/README.md)

适用目录：`ws63_test/`

这份文档现在只做一件事：

`手把手带你把现场演示跑通，而且明确按“先调主方案，再调备方案”的顺序来。`

你当前已经确定的策略是：

- 主方案：`SoftAP + 安卓手机 USB 共享网络`
- 备方案：`安卓手机热点 + STA`

这两套方案不要混在一起调。  
正确顺序是：

1. 先把双板基础链路调通
2. 再把主方案完整打通
3. 主方案跑稳以后，才去调备方案
4. 最后只保留一套现场主路径和一套现场备用路径

---

## 0. 你今天到底要做到什么

你最终要跑通的是这条链路：

```text
ai_studio 在线 AI 生图
    ->
轮廓提取 / G-Code 生成
    ->
通过 WiFi 发给发射板
    ->
发射板通过 SLE 发给接收板
    ->
接收板驱动振镜与激光执行
```

如果你今天时间有限，最低目标不是“一上来就一键比赛流程”，而是按下面顺序：

1. 双板启动正常
2. SLE 正常连接
3. 主方案下 `wifi_test.py --suite smoke` 通过
4. 主方案下 `ai_studio` 本地图发送通过
5. 主方案下 `ai_studio` 在线 AI 流程通过
6. 然后再去补备方案

---

## 1. 共用准备

### 1.1 硬件准备

你需要：

- 一块发射板
- 一块接收板
- 一台安卓手机
- 一台电脑
- 一根可靠的 USB 数据线
- 激光器、振镜、供电和接线已经确认

### 1.2 软件准备

如果你当前工作目录在 `/root/fbb_ws63/src`，下面命令都可以直接用。

安装依赖：

```bash
pip install PySide6 opencv-python pyserial requests numpy
```

如果你用 `PyQt5`：

```bash
pip install PyQt5 opencv-python pyserial requests numpy
```

### 1.3 安卓手机共用设置

无论主方案还是备方案，先把手机设置成下面这样：

- 关闭省电模式
- 关闭超级省电
- 保持手机插电
- 热点名和密码只用英文和数字
- 不要用中文、空格和特殊字符
- 演示期间不要让手机跑重负载应用

### 1.4 固件共用要求

接收板：

- 打开 `CONFIG_LASER_MARKER_RECEIVER`
- 关闭 `CONFIG_LASER_MARKER_TRANSMITTER`

发射板：

- 打开 `CONFIG_LASER_MARKER_TRANSMITTER`
- 关闭 `CONFIG_LASER_MARKER_RECEIVER`
- 打开 `CONFIG_LASER_WIFI_SOFTAP_ENABLE`

编译：

```bash
python3 build.py menuconfig ws63-liteos-app
python3 build.py ws63-liteos-app
```

烧录顺序：

1. 先烧接收板
2. 再烧发射板

---

## 2. 先做双板基础检查

这一步不通过，后面不要继续调手机网络。

### 2.1 看接收板日志

上电后，接收板应看到类似：

```text
WS63 Laser Marker - Receiver Board
[receiver] hardware init OK
[interpolator] task started
[safety] monitor task started
```

如果你没看到这些：

- 先检查是不是烧错固件
- 先检查接收板硬件初始化是否失败

### 2.2 看发射板日志

上电后，发射板应看到类似：

```text
WS63 Laser Marker - Transmitter Board
[transmitter] init OK
[laser tx] scanning...
```

如果没看到：

- 先检查是不是烧错固件
- 先检查发射板初始化是不是失败

### 2.3 看 SLE 是否连通

继续看日志，你应看到：

发射板：

```text
found LaserRX
connected!
```

接收板：

```text
SLE connected
```

如果没看到：

- 先不要怪 WiFi
- 先查双板无线链路
- 先确认两块板角色没烧反

### 2.4 用最小命令验证基础执行

如果你有串口工具，建议先发一组最小命令：

```gcode
$I
G90
M3 S200
G1 X10 Y10 F6000
M5
?
```

预期现象：

- `$I` 返回版本
- 每行返回 `ok`
- 振镜有动作
- `M3/M5` 能控制激光开关
- `?` 能返回状态

如果这一步不通过：

- 先检查 `uart_handler.c`
- 再检查 `gcode_processor.c`
- 再检查 `sle_client.c / sle_server.c / interpolator.c`

通过标准：

- 双板启动正常
- SLE 正常连接
- 最小动作集能跑通

只有这一步通过，才进入主方案。

---

## 3. 主方案：SoftAP + 安卓手机 USB 共享网络

这一章是你当前最重要的内容。  
这章必须先调通。  
没调通之前，不要去动备方案。

### 3.1 主方案拓扑

```text
安卓手机 --USB共享网络--> 电脑（负责上网调 AI）
电脑 --WiFi--> WS63 发射板 SoftAP（192.168.43.1）
WS63 发射板 --SLE--> 接收板
```

这条路线的关键思想是：

- `外网` 走手机 USB 共享网络
- `控制链路` 走 WS63 自己的 SoftAP

也就是说：

- 电脑既能上网
- 又能直连板子

这正是它适合做现场主方案的原因。

---

### 3.2 主方案第 1 步：把发射板切到 SoftAP

在 `menuconfig` 中确认：

- 打开 `CONFIG_LASER_MARKER_TRANSMITTER`
- 打开 `CONFIG_LASER_WIFI_SOFTAP_ENABLE`
- 选择 `CONFIG_LASER_WIFI_MODE_SOFTAP`

重新编译并烧录发射板。

预期现象：

- 发射板启动日志里不应该出现 `sta ready`
- 应该出现 `softap ready`

通过标准：

- 发射板明确运行在 `SoftAP` 模式

---

### 3.3 主方案第 2 步：看发射板 SoftAP 日志

上电后看发射板 UART0 日志。

你应该看到类似：

```text
[wifi gcode] wifi init ready
[wifi gcode] softap ready ssid=WS63_LaserTX ip=192.168.43.1 port=5000
[wifi gcode] tcp listen ready mode=SoftAP ip=192.168.43.1 port=5000
```

这一步的核心结论只有两个：

1. 热点真的起来了
2. TCP 服务真的监听了

如果没有 `softap ready`：

- 先查发射板 WiFi 初始化
- 先不要去碰 `ai_studio`

如果有 `softap ready` 但没有 `tcp listen ready`：

- 优先看 `wifi_gcode_server.c`
- 说明 WiFi 起了，但 TCP 服务没起来

---

### 3.4 主方案第 3 步：安卓手机打开 USB 共享网络

现在先别碰电脑 Wi-Fi。

先在安卓手机上做下面动作：

1. 打开 `移动数据`
2. 用 USB 线把手机连到电脑
3. 在手机里打开 `USB 共享网络`

常见路径：

- `设置 -> 连接与共享 -> USB共享网络`
- `设置 -> 热点与网络共享 -> USB共享网络`
- `设置 -> 更多连接 -> USB共享网络`

预期现象：

- 电脑上会多一张新的网络接口
- 电脑可以通过手机上外网

如果手机上没有这个选项：

- 很可能数据线不支持数据传输
- 先换数据线再试

---

### 3.5 主方案第 4 步：电脑连接 WS63 SoftAP

现在让电脑 Wi-Fi 连接发射板热点：

- SSID：`WS63_LaserTX`
- 密码：`ws63laser`

连接后，电脑通常会拿到：

- `192.168.43.x`

而发射板地址固定是：

- `192.168.43.1`

预期现象：

- 电脑已经连到 WS63 热点
- 同时 USB 共享网络仍然开着

这一步非常关键，因为后面：

- 访问板子走 `192.168.43.1`
- 访问 AI 接口走手机 USB 共享网络

---

### 3.6 主方案第 5 步：同时验证“能上网 + 能连板子”

在电脑上先做两个测试。

测试 1：板子可达

```bash
ping -c 3 192.168.43.1
```

测试 2：外网可达

```bash
curl -I https://www.google.com
```

预期现象：

- `192.168.43.1` 可以 ping 通
- 外网请求也能成功

如果板子通、外网不通：

- 大概率 USB 共享网络没成功

如果外网通、板子不通：

- 大概率电脑没真正连上 WS63 SoftAP

如果两边都不通：

- 先停下来，不要继续
- 先把网络层理顺

通过标准：

- 电脑同时满足“能访问 `192.168.43.1`”和“能访问外网”

---

### 3.7 主方案第 6 步：先不用 ai_studio，先打通纯 TCP

现在先不用 GUI，先走最简单的工具链。

运行：

```bash
python3 ws63_test/tools/wifi_bridge.py --host 192.168.43.1 -i
```

进入后，预期先看到欢迎信息：

```text
WS63 Laser Marker WiFi
Grbl 1.1f ['$' for help]
```

然后依次输入：

```text
$I
$WIFI?
?
G90
M5
```

预期现象：

- `$I` 返回版本
- `$WIFI?` 返回 WiFi 状态
- `$WIFI?` 中应看到 `MODE=SoftAP`
- `$WIFI?` 中应看到 `SLE=1`
- `?` 能返回状态
- `G90` 和 `M5` 返回 `ok`

如果 `$WIFI?` 里 `SLE=0`：

- 说明 SoftAP 是通的
- 但双板无线链路没准备好

如果这里都过了，说明：

- 主方案网络已经能到发射板
- 发射板 TCP 文本入口工作正常

---

### 3.8 主方案第 7 步：跑自动化 WiFi 验证

运行：

```bash
python3 ws63_test/tools/wifi_test.py --host 192.168.43.1 --suite smoke
python3 ws63_test/tools/wifi_test.py --host 192.168.43.1 --suite all
```

预期现象：

- `intro_check` 通过
- `wifi_status` 通过
- `smoke` 通过
- `square` 通过
- `repeat` 通过
- `status_stability` 通过

如果 `smoke` 都没过：

- 不要开 `ai_studio`
- 说明主方案的底层 TCP 还没打稳

通过标准：

- `wifi_test.py --suite all` 全部 `PASS`

---

### 3.9 主方案第 8 步：启动 ai_studio

先配置 API Key，推荐用环境变量：

```bash
export GOOGLE_API_KEY="你的真实GoogleAPIKey"
```

然后启动：

```bash
python3 -m ws63_test.ai_studio.main
```

注意：

- 启动前先退出 `wifi_bridge.py`
- 不要让别的 TCP 客户端占着板子

因为当前板端只允许一个上游主机有效控制。

---

### 3.10 主方案第 9 步：先做本地图闭环

这是最重要的一步之一。  
先别一上来就点“一键比赛流程”。

在 `ai_studio` 里这样做：

1. 进入 `步骤一：连接`
2. 选择 `WiFi SoftAP`
3. 确认 IP 为 `192.168.43.1`
4. 点击 `连接设备`
5. 进入 `步骤二：创作`
6. 导入一张本地图片
7. 检查原图和轮廓图是否显示
8. 进入 `步骤三：雕刻`
9. 生成 G-Code
10. 发送到设备

预期现象：

- 连接成功
- 系统消息板有 `WiFi 连接成功`
- 原图正常显示
- 轮廓图正常显示
- G-Code 正常生成
- 日志里出现 `[TX]` / `[RX] ok`
- 真机开始动作

如果这一步过不了：

- 不要碰 AI 生图
- 说明问题还在 GUI 到板子的链路层

通过标准：

- `本地图 -> 轮廓 -> G-Code -> WiFi 下发 -> 真机执行` 全流程跑通

---

### 3.11 主方案第 10 步：最后才做在线 AI 闭环

现在才开始测：

- 在线 AI 生图
- 轮廓提取
- G-Code
- WiFi 下发

建议先用内置模板，不要自由发挥。

推荐操作：

1. 选择模板
2. 点击“载入模板”
3. 小幅修改 Prompt
4. 确认设备已经连接
5. 点击 `⚡ 一键比赛流程`

预期现象：

顶部任务状态条会依次变化：

```text
等待任务
-> AI 图像生成中
-> 轮廓提取中
-> G-Code 生成中
-> 下发到设备中
-> 任务执行完成
```

同时你应看到：

- AI 图像生成成功
- 原图显示正常
- 轮廓图生成成功
- G-Code 成功生成
- WiFi 正常发送
- 真机执行成功

如果 AI 图生出来了，但轮廓失败：

- 不要先怀疑网络
- 大概率是 Prompt 太复杂

如果本地图能发，AI 发不了：

- 大概率是外网或 API Key 问题

通过标准：

- `Prompt -> AI 生图 -> 轮廓 -> G-Code -> WiFi 下发 -> 真机执行` 跑通

---

### 3.12 主方案完成标准

只有下面 5 条都通过，主方案才算完成：

- 双板启动正常
- 发射板 SoftAP 正常
- `wifi_test.py --suite all` 通过
- `ai_studio` 本地图流程通过
- `ai_studio` 在线 AI 流程通过

主方案没完成之前，不进入备方案。

---

## 4. 备方案：安卓手机热点 + STA

只有主方案已经完成，才调这一章。

这章的目的不是替代主方案，而是给你一条无线备用路径。

### 4.1 备方案拓扑

```text
安卓手机热点
   ├── 电脑
   └── 发射板（STA）

电脑 -> 发射板 TCP -> SLE -> 接收板
```

和主方案相比，备方案更整洁，但变量更多：

- 热点频段
- DHCP 分配
- 发射板入网
- 实际 IP 变化

所以它必须放在主方案后面调。

---

### 4.2 备方案第 1 步：设置安卓手机热点

先把安卓手机热点开起来。

建议参数：

- 热点名：英文
- 密码：英文数字
- 频段：优先 `2.4GHz`
- 关闭“无设备自动关闭热点”
- 手机保持插电

预期现象：

- 手机热点稳定存在
- 电脑能看到这个热点

如果手机支持选择频段，第一轮一定先选 `2.4GHz`，不要默认赌 `5GHz`。

---

### 4.3 备方案第 2 步：把发射板切到 STA

在 `menuconfig` 中确认：

- 打开 `CONFIG_LASER_MARKER_TRANSMITTER`
- 打开 `CONFIG_LASER_WIFI_SOFTAP_ENABLE`
- 选择 `CONFIG_LASER_WIFI_MODE_STA`
- 正确填写手机热点的 `SSID` 和密码

重新编译并烧录发射板。

预期现象：

- 发射板后续不会出现 `softap ready`
- 而是应该出现 `sta ready`

---

### 4.4 备方案第 3 步：看发射板 STA 日志

上电后看发射板日志。

应看到类似：

```text
[wifi gcode] sta ready ssid=你的热点名 ip=192.168.x.x port=5000
[wifi gcode] tcp listen ready mode=STA ip=192.168.x.x port=5000
```

把实际 IP 抄下来：

- 发射板 STA IP：________________

这一步的预期现象非常明确：

- 发射板成功连上手机热点
- 发射板成功拿到 IP
- TCP 服务成功监听

如果只看到 WiFi 初始化，没有 `sta ready`：

- 先查热点名、密码、频段
- 先不要开 `ai_studio`

---

### 4.5 备方案第 4 步：让电脑连接同一个手机热点

现在让电脑也连同一个手机热点。

然后做两个测试：

测试 1：

```bash
ping -c 3 <发射板STA_IP>
```

测试 2：

电脑确认还能上外网。

预期现象：

- 发射板 IP 可达
- 外网也可用

如果电脑能上外网但 ping 不到发射板：

- 说明发射板没真正入网或 IP 记错了

---

### 4.6 备方案第 5 步：先做纯 TCP 验证

先别开 `ai_studio`，先做 TCP 验证。

运行：

```bash
python3 ws63_test/tools/wifi_bridge.py --host <发射板STA_IP> -i
python3 ws63_test/tools/wifi_test.py --host <发射板STA_IP> --suite smoke
python3 ws63_test/tools/wifi_test.py --host <发射板STA_IP> --suite all
```

预期现象：

- 欢迎信息正常
- `$WIFI?` 中 `MODE=STA`
- `$WIFI?` 中 `SLE=1`
- 自动化测试通过

如果这里没过：

- 先别开 `ai_studio`
- 先把 STA 底层跑稳

---

### 4.7 备方案第 6 步：先做 ai_studio 本地图流程

现在打开：

```bash
python3 -m ws63_test.ai_studio.main
```

在 GUI 中：

1. 进入 `步骤一：连接`
2. 选择 `WiFi STA`
3. 输入发射板实际 STA IP
4. 点击 `连接设备`
5. 先导入本地图片
6. 生成轮廓
7. 生成 G-Code
8. 发送到设备

预期现象：

- 连接成功
- 日志中有 `WiFi 连接成功`
- 真机成功执行

---

### 4.8 备方案第 7 步：最后做在线 AI 流程

现在再做：

- 选择模板
- 输入 Prompt
- 点击 `一键比赛流程`

预期现象：

- AI 成功出图
- 轮廓成功
- G-Code 成功
- WiFi 下发成功
- 真机执行成功

通过标准：

- `wifi_test.py` 通过
- `ai_studio` 本地图流程通过
- `ai_studio` 在线 AI 流程通过

---

### 4.9 备方案完成标准

只有下面 4 条都过了，备方案才算完成：

- 发射板成功连入手机热点
- `wifi_test.py --suite all` 通过
- `ai_studio` 本地图流程通过
- `ai_studio` 在线 AI 流程通过

---

## 5. 现场切换策略

你现场只需要记住下面这条原则：

### 5.1 默认走主方案

默认演示始终走：

- `SoftAP + 安卓手机 USB 共享网络`

### 5.2 什么情况下切到备方案

只有下面情况才切：

- USB 共享网络不稳定
- 电脑始终无法同时做到“上网 + 连 SoftAP”
- 手机 USB 识别异常

### 5.3 切换步骤

从主方案切到备方案：

1. 关闭 `ai_studio`
2. 退出其他 TCP 工具
3. 让发射板换成 `STA` 固件
4. 打开安卓热点
5. 看发射板日志，抄下新 IP
6. 电脑连同一热点
7. 先跑 `wifi_test.py --suite smoke`
8. 通过后再开 `ai_studio`

---

## 6. 最终彩排清单

### 6.1 主方案彩排

- [ ] 双板启动正常
- [ ] SLE 正常连接
- [ ] 发射板 SoftAP 正常
- [ ] 电脑同时能上网和访问 `192.168.43.1`
- [ ] `wifi_test.py --suite all` 通过
- [ ] `ai_studio` 本地图流程通过
- [ ] `ai_studio` 在线 AI 流程通过
- [ ] 急停验证通过

### 6.2 备方案彩排

- [ ] 发射板 STA 正常入网
- [ ] 电脑和发射板在同一热点下
- [ ] 发射板实际 IP 已记录
- [ ] `wifi_test.py --suite all` 通过
- [ ] `ai_studio` 本地图流程通过
- [ ] `ai_studio` 在线 AI 流程通过

### 6.3 现场演示前最后确认

- [ ] 手机已插电
- [ ] 电脑电量充足
- [ ] 数据线可靠
- [ ] 当前网络方案已确认
- [ ] 发射板 IP 已确认
- [ ] 已经清掉其他 TCP 客户端
- [ ] 最近一次彩排成功

---

## 7. 如果要把问题交给下一个 AI

如果后面你还要交给新的 AI 排障，最少给它这 4 份文档：

1. [README.md](/root/fbb_ws63/src/ws63_test/README.md)
2. [CODE_ARCHITECTURE.md](/root/fbb_ws63/src/ws63_test/CODE_ARCHITECTURE.md)
3. [WS63_DEBUG_QUICKSTART.md](/root/fbb_ws63/src/ws63_test/WS63_DEBUG_QUICKSTART.md)
4. [README.md](/root/fbb_ws63/src/ws63_test/ai_studio/README.md)

再补这些运行材料最有帮助：

- 发射板日志
- 接收板日志
- 当前网络方案说明
- 失败现场截图
- 原图
- 轮廓图
- `output.gcode`
- `wifi_test.py` 结果
