# WS63 项目进度与交接报告

更新时间：`2026-04-18`
适用目录：`ws63_test/`

这份报告的目标是让下一个 AI 或人接手时，不需要重新梳理上下文，就能知道：

1. 这个项目现在做到哪一步了
2. 哪些功能已经验证通过
3. 哪些改动已经落地到代码和文档
4. 还剩什么待做
5. 接手后最推荐先做什么

---

## 1. 总体状态

当前项目可以分成 3 个层面来看：

### 1.1 串口主链路

状态：`已完成并已验证`

用户前提说明：
- 串口部分此前已经全部验证与完成

当前默认认知：
- 发射板 `UART1` 文本 G-Code 入口可用
- 发射板到接收板 `SLE` 主链路可用
- 接收板执行运动、状态回传的基础链路可用

### 1.2 WiFi SoftAP 链路

状态：`已基本打通，功能可验收，长时自动化稳定性仍有小概率抖动`

已确认通过的能力：
- 发射板可启动 `SoftAP`
- 电脑/手机可连接热点
- TCP 可连接 `192.168.43.1:5000`
- 欢迎语正常返回
- `$WIFI?` 正常
- `?` 状态查询正常
- 基本 G-Code 下发正常
- `smoke` 测试通过
- `all --cycles 5` 大部分通过

### 1.3 WiFi STA 链路

状态：`文档已准备，尚未完成系统联调验收`

当前已完成：
- `STA` 菜单配置说明已补齐
- `STA` 上板调试步骤已整理到文档

当前未完成：
- 未看到完整的 `STA` 模式实测通过记录
- 未沉淀 `STA` 下的自动化通过结果

---

## 2. 本轮已落地的代码与文档

### 2.1 代码改动

本轮与 WiFi 直接相关的改动主要在以下文件：

- `transmitter/wifi_gcode_server.c`
- `tools/wifi_bridge.py`
- `tools/wifi_client.py`
- `tools/wifi_test.py`

#### A. 板端增加“单客户端占用”明确提示

文件：
- `transmitter/wifi_gcode_server.c`

已做内容：
- 当已有一个上游客户端占用会话时，新的客户端连接会收到：

```text
WS63 Laser Marker WiFi
[MSG:busy another upstream host is connected]
error:busy
```

- 然后立即断开

意义：
- 避免出现“connect 成功但没有欢迎语、没有响应”的模糊现象
- 方便上位机和自动化脚本判断“当前是被旧客户端占着”

#### B. `wifi_bridge.py` 对 busy 场景做了更友好的处理

文件：
- `tools/wifi_bridge.py`

已做内容：
- 如果欢迎信息阶段就收到 `busy`，脚本会明确提示：
  `当前板端已有其他上游客户端连接，请先断开旧连接后再重试。`

意义：
- 让人工联调时更容易判断问题，不再把 `busy` 误以为是热点或 TCP 本身失败

#### C. `wifi_client.py` 增加了重连能力

文件：
- `tools/wifi_client.py`

已做内容：
- 记住上一次成功连接的 `host/port`
- 新增 `reconnect()` 方法

意义：
- 为自动化里的“收到 busy 时自动重试一次”提供基础能力

#### D. `wifi_test.py` 增强了稳定性与可读性

文件：
- `tools/wifi_test.py`

已做内容：
- 每个测试项单独建立一次连接，不再整套共用同一条 TCP 会话
- `intro_check` 在遇到 `busy` 时自动重试一次
- 每个测试项结束后增加短暂等待，让板端观察到 TCP 断开并回到监听态

意义：
- 降低测试项切换时的连接残留影响
- 提高自动化可诊断性

---

## 3. 本轮已落地的文档

### 3.1 新增文档

- `WIFI_IMPLEMENTATION_GUIDE.md`
- `PROJECT_PROGRESS_HANDOFF_REPORT.md`（本文件）

### 3.2 已更新文档

- `README.md`

更新重点：
- 补充了 WiFi 调试顺序
- 补充了 `SoftAP` 的默认 SSID、密码、默认信道
- 补充了“`channel=13` 可能导致某些设备搜不到热点”的说明
- 补充了“已有客户端占用时会返回 `busy`”的说明

---

## 4. 已完成的验证结果

下面是本轮可以确认的、已经被验证过的结果。

### 4.1 发射板 SoftAP 启动日志正常

典型通过日志：

```text
[wifi gcode] wifi init ready
[wifi gcode] wifi event callback ready
[wifi gcode] softap state available
[wifi gcode] softap ready ssid=WS63_LaserTX ip=192.168.43.1 port=5000 channel=6
[wifi gcode] tcp listen ready mode=SoftAP ip=192.168.43.1 port=5000
```

说明：
- WiFi 子系统正常
- 热点正常
- TCP 监听正常

### 4.2 手机与电脑都已能看到并连接热点

确认结果：
- 手机可看到热点
- Windows 电脑已能成功连接 `WS63_LaserTX`

经验结论：
- 默认 `channel=13` 可能导致部分电脑看不到热点
- 改为 `channel=6` 后，Windows 侧能正常看到并连接

### 4.3 手工交互验证通过

Windows 上使用 `wifi_bridge.py` 的成功现象：

```text
WS63 Laser Marker WiFi
Grbl 1.1f ['$' for help]
[MSG:WiFi SoftAP WS63_LaserTX]
[MSG:Use $WIFI? for link status]
[MSG:One upstream host at a time]
```

并且：
- `/status` 成功返回
- `/wifi` 成功返回

典型 `$WIFI?` 返回：

```text
[WIFI:MODE=SoftAP,SSID=WS63_LaserTX,IF=ap0,IP=192.168.43.1,NET=1,TCP=1,CLI=1,APSTA=1,STALINK=0,SLE=1,REASON=0]
```

说明：
- `MODE=SoftAP`
- `NET=1`
- `TCP=1`
- `CLI=1`
- `SLE=1`

结论：
- `SoftAP + TCP + 状态查询 + SLE 就绪` 已确认打通

### 4.4 `smoke` 自动化通过

已看到的通过结果：

```text
PASS intro_check
PASS wifi_status
PASS smoke
=== 3/3 PASS ===
```

结论：
- `SoftAP` 模式下最小验收路径已经通过

### 4.5 `all --cycles 5` 大部分通过

本轮不同时间窗口下看到过两类典型结果：

#### 结果 A

```text
=== 29/30 PASS ===
```

#### 结果 B

```text
=== 26/30 PASS ===
```

失败模式主要表现为：
- `busy`
- `connection closed by remote`
- `WinError 10054`

这类失败的共同特征是：
- 多发生在测试项切换、短连接重建的瞬间
- 后续其他项又能继续 PASS
- 不像是热点整体崩溃，更像是 TCP 会话切换时序问题

结论：
- `SoftAP` 功能层面可认为基本验收通过
- 但长时自动化稳定性仍未完全收敛

---

## 5. 当前最重要的结论

如果只看“功能有没有打通”，当前可以下结论：

### 5.1 可以确认通过的部分

1. 串口主链路：已完成
2. 发射板 `SoftAP` 启动：通过
3. Windows 电脑连接热点：通过
4. TCP 建连：通过
5. 欢迎语：通过
6. `$WIFI?`：通过
7. `?` 状态查询：通过
8. 基本 G-Code 下发：通过
9. `smoke`：通过

### 5.2 尚未完全收口的部分

1. `all --cycles 5` 并非每次都稳定 `30/30 PASS`
2. 测试项切换时偶发：
   - `busy`
   - `remote close`
   - `10054`
3. `STA` 还未完成系统性实测验收

---

## 6. 当前剩余待落地事项

下面这些是最值得下一个接手人继续推进的。

### 6.1 第一优先级：定位 TCP 会话切换时序问题

现象：
- 自动化中偶发 `busy`
- 自动化中偶发 `WinError 10054`
- 单个功能项能通过，但整套长时间回归不总是稳定

建议排查方向：
- 对照失败时刻的发射板串口日志
- 重点看：
  - `client connected`
  - `client disconnected`
  - `reject extra client: upstream already occupied`
  - `softap state unavailable`
  - 是否有重启/异常输出

目标：
- 搞清楚是板端 TCP 会话释放慢
- 还是 Windows 侧连接切换太快
- 还是 WiFi 服务在个别时刻主动断会话

### 6.2 第二优先级：完成 STA 模式实测验收

当前状态：
- 文档已写好
- 菜单和操作步骤已整理
- 但还没有完整的实测闭环记录

建议目标：
1. 实测 `STA` 启动日志
2. 实测连接同一热点
3. 实测 `wifi_bridge.py --host 实际IP -i`
4. 实测 `smoke`
5. 实测 `all`

### 6.3 第三优先级：把自动化结果沉淀为正式验收记录

当前状态：
- 已有命令与过程
- 但还缺正式的项目级记录

建议沉淀内容：
- SoftAP `smoke` 截图或日志
- SoftAP `all --cycles 5` 日志
- 失败项说明
- 当前已知风险与解释

---

## 7. 接手人最应该先看哪些文件

建议按下面顺序看：

1. `WIFI_IMPLEMENTATION_GUIDE.md`
2. `README.md`
3. `transmitter/wifi_gcode_server.c`
4. `tools/wifi_bridge.py`
5. `tools/wifi_client.py`
6. `tools/wifi_test.py`

---

## 8. 接手后最推荐的第一组操作

如果下一个人接手后要先快速确认现状，建议直接做这组操作。

### 8.1 先确认板端日志

确认发射板是否仍能输出：

```text
[wifi gcode] wifi init ready
[wifi gcode] softap ready ssid=WS63_LaserTX ip=192.168.43.1 port=5000 channel=6
[wifi gcode] tcp listen ready mode=SoftAP ip=192.168.43.1 port=5000
```

### 8.2 再做手工联调

Windows 本地目录最少放这几个文件：

```text
wifi_bridge.py
wifi_client.py
wifi_test.py
```

然后执行：

```powershell
py -3 .\wifi_bridge.py --host 192.168.43.1 -i
```

确认：
- 欢迎语
- `/wifi`
- `/status`

### 8.3 再做最小自动化

```powershell
py -3 .\wifi_test.py --host 192.168.43.1 --suite smoke
```

目标：

```text
=== 3/3 PASS ===
```

### 8.4 最后再做长时自动化

```powershell
py -3 .\wifi_test.py --host 192.168.43.1 --suite all --cycles 5
```

接手时要特别记住：
- 如果这里没到 `30/30 PASS`，优先先看串口日志
- 不要先怀疑热点没起

---

## 9. 风险与注意事项

### 9.1 已知热点信道问题

- 默认 `channel=13` 时，部分电脑看不到热点
- 现场联调建议固定用 `channel=6`

### 9.2 已知单客户端占用机制

- 同一时刻只允许一个上游客户端会话
- 第二个客户端会收到：

```text
[MSG:busy another upstream host is connected]
error:busy
```

这属于预期行为，不是故障。

### 9.3 自动化长时回归仍存在偶发抖动

当前表现：
- `29/30 PASS` 或 `26/30 PASS`

需要继续定位：
- 会话释放
- 会话切换
- 远端主动断开连接

---

## 10. 当前建议的项目状态结论

可以给项目当前阶段下一个比较稳妥的结论：

### 已完成

- 串口链路：完成
- SoftAP 功能：完成
- Windows 电脑接入与手工交互：完成
- `smoke` 自动化：完成

### 基本完成但仍建议继续优化

- `all --cycles 5` 长时自动化稳定性

### 未完成

- `STA` 实测验收
- 项目级正式验收记录沉淀

---

## 11. 一句话交接结论

`当前项目已经从“串口可用”推进到“SoftAP 已基本打通并完成最小自动化验收”，下一位接手人最值得优先做的是：继续定位 WiFi 长时回归中的 TCP 会话切换抖动，并完成 STA 模式的系统性验收。`
