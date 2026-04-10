# WS63 WiFi 工具链 — 傻瓜式调试手册

> 目标读者：拿到板子后 5 分钟跑通 WiFi 链路。
> 零外部 Python 依赖，只要有 `python3` 就能用。

---

## 0. 准备清单

| 项目 | 说明 |
|------|------|
| 发射板 | 烧好 **发射板固件**（`CONFIG_LASER_MARKER_TRANSMITTER` + WiFi 开启） |
| 接收板 | 烧好 **接收板固件**（`CONFIG_LASER_MARKER_RECEIVER`）并已上电 |
| PC     | 任意系统，装有 `python3`（≥3.7） |
| WiFi   | PC 连上发射板热点 **或** 与发射板在同一路由器下 |

**SoftAP 模式**（比赛现场推荐）：
- 发射板自动开热点 `WS63_LaserTX`，密码 `ws63laser`
- PC 连该热点，板端 IP 固定 `192.168.43.1`

**STA 模式**（日常开发）：
- 发射板连你的路由器/手机热点
- 看调试串口日志找 `sta ready ... ip=xxx.xxx.xxx.xxx`

---

## 1. 第一步：用桥接脚本验证连通性

这是最快的验证方式，一条命令就能知道 WiFi 链路通不通：

```bash
cd src/ws63_test/tools
python3 wifi_bridge.py --host 192.168.43.1 -i
```

**正常输出** ——

```
连接 192.168.43.1:5000 ...
  WS63 Laser Marker WiFi
  Grbl 1.1f ['$' for help]
  [MSG:WiFi SoftAP WS63_LaserTX]
  [MSG:One upstream host at a time]
  WiFi: SoftAP IP=192.168.43.1 SLE=✓

WS63 WiFi Bridge 交互模式
输入 G-Code 命令，按 Enter 发送
特殊命令: /status  /wifi  /quit  /help

ws63>
```

**如果卡在"连接..."** → 见下方 [故障排查](#5-故障排查表)

连上后试几个命令：

```
ws63> $I                    ← 查版本
  [VER:1.1f.WS63:]
  [OPT:V,15,128]
  ok

ws63> /wifi                 ← 查 WiFi 状态
  [WIFI:MODE=SoftAP,...,SLE=1,...]
  模式=SoftAP  IP=192.168.43.1  SLE=✓

ws63> /status               ← 查运动状态
  <Idle|MPos:0.000,0.000,0.000|FS:0,0>
  状态=Idle  X=0.000  Y=0.000

ws63> G90                   ← 发个 G-Code
  ok

ws63> /quit
```

看到 `ok` 就说明 WiFi → 发射板 → SLE → 接收板 全链路通了。

---

## 2. 第二步：跑自动化验证

```bash
# 最小验证 (30 秒)
python3 wifi_test.py --host 192.168.43.1 --suite smoke

# 完整验证 (约 2 分钟)
python3 wifi_test.py --host 192.168.43.1 --suite all

# 稳定性验证 + 留档 (约 10 分钟)
python3 wifi_test.py --host 192.168.43.1 --suite all --cycles 5 --report-json wifi_result.json
```

**正常输出** ——

```
... PASS intro_check (0.12s) 5 lines
... PASS wifi_status (0.08s) [WIFI:MODE=SoftAP,...]
... PASS smoke (1.23s) <Idle|MPos:20.000,10.000,...>
... PASS square (2.45s) 14 cmds ok
... PASS repeat (5.67s) 10 rounds ok
... PASS status_stability (1.89s) 20 queries ok
=== 6/6 PASS ===
```

任何一行显示 `FAIL` 的，看 detail 列定位原因。

---

## 3. 第三步：启动网页控制台

```bash
python3 wifi_console.py
```

然后浏览器打开 **http://localhost:8080**。

### 操作流程

```
① 输入 IP → 点"连接"
     ↓
② 看左侧状态面板亮灯：
   - 网络 🟢  TCP 🟢  SLE 🟢 → 全部就绪
   - SLE 灰色 → 接收板未连上，只能发查询不能发运动命令
     ↓
③ 在命令框输入 G-Code 回车发送
   或在多行文本框粘贴整段 G-Code → 点"发送任务"
     ↓
④ 观察日志面板：
   蓝色 = 你发的   绿色 = ok   红色 = error   橙色 = 状态
```

### 快捷按钮说明

| 按钮 | 作用 |
|------|------|
| `$I` | 查固件版本 |
| `$CAP?` | 查能力画像（比赛用） |
| `$WIFI?` | 查 WiFi/SLE 状态 |
| `$G` | 查当前 G-Code 模态 |
| `?` | 查运动位置和状态 |
| `G90` | 切绝对坐标 |
| `G92` | 设当前位置为原点 |
| `M5` | 关激光 |
| `🛑 急停` | 发 M5 + 自动刷状态 |
| `🧪 Smoke` | 跑 smoke 预置任务 |

---

## 4. 第四步：从文件发送 G-Code

```bash
# 直接发文件
python3 wifi_bridge.py --host 192.168.43.1 --file demo.gcode

# 管道方式
cat demo.gcode | python3 wifi_bridge.py --host 192.168.43.1
```

输出每行的发送结果：

```
[1/42] G90 -> ok
[2/42] G92 -> ok
[3/42] M3 S200 -> ok
...
完成: 42 行已发送, 0 个错误
```

---

## 5. 故障排查表

### 连不上

| 现象 | 原因 | 解决 |
|------|------|------|
| `connect ... failed: Connection refused` | 板端 TCP 服务未启动 | 看调试串口有没有 `tcp listen ready`，没有说明 WiFi 初始化失败 |
| `connect ... failed: Network is unreachable` | PC 没连上发射板 WiFi | SoftAP 模式：先连 `WS63_LaserTX` 热点 |
| `connect ... failed: Connection timed out` | IP 不对或防火墙 | STA 模式：看串口日志确认实际 IP |
| 连上但所有命令 `error:2` | SLE 未就绪 | 接收板未上电或距离太远，查 `$WIFI?` 看 `SLE=0` |

### 命令超时

| 现象 | 原因 | 解决 |
|------|------|------|
| `timeout waiting for ok` | 板端 SLE 队列满 | 等接收板消化完当前命令再发；检查接收板是否卡死 |
| `timeout waiting for status` | `?` 查询无回复 | 板端可能正在处理长运动指令，加大 `--timeout` |
| 命令偶尔超时 | WiFi 信号弱 | 靠近发射板；SoftAP 模式干扰少更稳定 |

### 网页控制台问题

| 现象 | 原因 | 解决 |
|------|------|------|
| 页面打不开 | 后端没启动 | 确认 `python3 wifi_console.py` 在运行 |
| 灯全灰不更新 | 未连接或连接已断 | 点"连接"按钮 |
| 进度条不动 | 旧版本 bug（已修复） | 确认用最新代码 |

---

## 6. 各工具参数速查

### wifi_bridge.py

```
--host       板端 IP          默认 192.168.43.1
--port       TCP 端口         默认 5000
--file/-f    G-Code 文件路径
--interactive/-i  交互模式    (默认生成会话日志)
--timeout    命令超时秒数     默认 5
--log-file   日志文件路径
-v           详细输出
```

### wifi_test.py

```
--host       板端 IP          默认 192.168.43.1
--port       TCP 端口         默认 5000
--suite      测试套件         smoke/square/repeat/status/all
--rounds     repeat 轮数      默认 10
--cycles     整体循环次数     默认 1
--timeout    命令超时秒数     默认 5
--report-json  JSON 报告路径
-v           详细输出
```

### wifi_console.py

```
--web-port   本地 HTTP 端口   默认 8080
--bind       绑定地址         默认 0.0.0.0
-v           详细输出
```

---

## 7. 文件清单

```
tools/
├── wifi_client.py      共享传输层 (其他三个都 import 它)
├── wifi_console.py     网页控制台后端
├── wifi_console.html   网页控制台前端
├── wifi_test.py        TCP 自动化验证
├── wifi_bridge.py      命令行桥接脚本
├── uart_auto_test.py   UART 自动测试 (已有，不动)
└── stress_test.py      UART 压力测试 (已有，不动)
```

---

## 8. 比赛现场 30 秒演示脚本

如果评委只给你 30 秒展示 WiFi 能力：

```bash
# 终端 1：启动网页控制台
python3 wifi_console.py

# 终端 2：跑 smoke 验证（边跑边给评委看网页）
python3 wifi_test.py --host 192.168.43.1 --suite smoke
```

在网页上给评委展示：
1. 点连接 → 灯全绿
2. 点 `$CAP?` → 展示能力画像
3. 点 Smoke 测试 → 看进度条跑完
4. 指着日志面板说："每条命令实时回 ok，全链路闭环"

---

> **金句**：WiFi 连不上先看串口日志，命令报错先看 `$WIFI?` 的 `SLE` 字段。
