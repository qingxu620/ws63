# WS63 WiFi 上板操作手册

适用目录：`ws63_test/`

这份文档只讲 3 件事：

1. `menuconfig` 菜单怎么选
2. 怎么编译、怎么下载
3. 下载后应该看到什么现象，怎么判断是否调通

默认前提：
- 串口链路你们已经验证过
- 接收板固件已经能正常工作
- 现在重点只放在发射板 `WiFi TCP` 入口

建议顺序：

1. 先调 `SoftAP`
2. `SoftAP` 通了以后再调 `STA`

---

## 1. 先说结论

### 1.1 SoftAP 调通后，你应该看到

1. 发射板串口日志里出现：
   - `wifi init ready`
   - `softap ready ssid=WS63_LaserTX ip=192.168.43.1 port=5000`
   - `tcp listen ready mode=SoftAP ip=192.168.43.1 port=5000`
2. PC 能连接热点 `WS63_LaserTX`
3. 运行 `wifi_bridge.py` 能连上 `192.168.43.1:5000`
4. 输入 `$WIFI?`、`?`、`G90` 都有正常回复

### 1.2 STA 调通后，你应该看到

1. 发射板串口日志里出现：
   - `sta enabled, target ssid=你的热点名`
   - `sta ready ssid=你的热点名 ip=实际IP port=5000`
   - `tcp listen ready mode=STA ip=实际IP port=5000`
2. PC 和发射板在同一个路由器或手机热点下
3. 运行 `wifi_bridge.py` 能连上日志打印出来的实际 IP
4. 输入 `$WIFI?`、`?`、`G90` 都有正常回复

---

## 2. 编译和下载前你要知道的事

### 2.1 WiFi 功能只在发射板上开

WiFi 是发射板功能，不是接收板功能。

所以调 WiFi 时：
- 发射板要选 `CONFIG_LASER_MARKER_TRANSMITTER`
- 接收板不要选 `CONFIG_LASER_MARKER_RECEIVER`

### 2.2 如果你只想看 WiFi 是否起来

只烧发射板也可以看到：
- 热点是否起来
- TCP 是否监听
- `$WIFI?` 是否有网络状态

### 2.3 如果你还想验证运动是否真的走通

那就需要：
- 接收板也已经烧好接收固件
- 发射板和接收板的 SLE 已经正常连上

否则你能看到 WiFi 通，但业务命令可能会因为 `SLE=0` 无法真正驱动动作。

---

## 3. SoftAP 操作步骤

这部分你可以完全照着做。

### 3.1 第一步：选择菜单

在仓库根目录执行：

```bash
python3 build.py menuconfig ws63-liteos-app
```

进菜单后，只做下面这些选择。

#### 3.1.1 先选板卡角色

在 `Board Role (板卡角色)` 里：

- 选 `Support Transmitter Board (接收板不要选)`

也就是最终效果应当是：

```text
CONFIG_LASER_MARKER_TRANSMITTER=y
CONFIG_LASER_MARKER_RECEIVER=n
```

#### 3.1.2 再进发射配置

进入：

```text
Transmitter Configuration (发射配置)
```

把下面这项打开：

```text
Support WiFi (启用WiFi)
```

也就是：

```text
CONFIG_LASER_WIFI_SOFTAP_ENABLE=y
```

#### 3.1.3 再进 WiFi Configuration

进入：

```text
WiFi Configuration (WiFi设置)
```

在 `WiFi Mode (模式)` 里选择：

```text
Support WiFi SoftAP Mode (热点模式)
```

不要选 `STA`。

也就是最终要变成：

```text
CONFIG_LASER_WIFI_MODE_SOFTAP=y
CONFIG_LASER_WIFI_MODE_STA=n
```

#### 3.1.4 SoftAP 参数先保持默认

第一轮不要改参数，直接用默认值：

```text
Set SoftAP SSID (热点名)      = WS63_LaserTX
Set SoftAP Password (热点密码) = ws63laser
Choose SoftAP Channel (信道)   = 13
Choose TCP Port (端口)         = 5000
```

对应结果：

```text
CONFIG_LASER_WIFI_SOFTAP_SSID="WS63_LaserTX"
CONFIG_LASER_WIFI_SOFTAP_PSK="ws63laser"
CONFIG_LASER_WIFI_SOFTAP_CHANNEL=13
CONFIG_LASER_WIFI_TCP_PORT=5000
```

#### 3.1.5 保存并退出

保存配置后退出 `menuconfig`。

这一步完成后的正确状态是：
- 发射板角色已选对
- WiFi 已开启
- 模式是 `SoftAP`
- 默认热点参数没动

### 3.2 第二步：编译

在仓库根目录执行：

```bash
python3 build.py ws63-liteos-app
```

如果你怀疑之前配置残留，可以先清理再编译：

```bash
python3 build.py -c ws63-liteos-app
python3 build.py ws63-liteos-app
```

编译成功后，重点看产物：

```text
output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg
```

这个 `fwpkg` 就是后面下载要用的固件包。

### 3.3 第三步：下载到板子

仓库里没有固定死某一种命令行烧录方式，所以你按你们当前环境的常用方式下载即可，通常是：

1. 用 DevEco 的 Upload
2. 或厂商提供的烧录工具

你要选的固件文件就是：

```text
output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg
```

下载对象：
- 如果现在只验证 WiFi 热点和 TCP，下载发射板即可
- 如果还要验证运动闭环，接收板也要烧好接收固件

建议顺序：

1. 接收板先烧好接收固件
2. 发射板再烧这次的发射固件

### 3.4 第四步：上电后先只看发射板日志

现在先不要连 PC 热点，也不要跑脚本。

只看发射板调试串口，正常情况下应该看到类似日志：

```text
[wifi gcode] wifi init ready
[wifi gcode] wifi event callback ready
[wifi gcode] softap state available
[wifi gcode] softap ready ssid=WS63_LaserTX ip=192.168.43.1 port=5000 channel=13
[wifi gcode] tcp listen ready mode=SoftAP ip=192.168.43.1 port=5000
```

你要重点核对 3 件事：

1. 有没有 `wifi init ready`
2. 有没有 `softap ready ssid=WS63_LaserTX ip=192.168.43.1 port=5000`
3. 有没有 `tcp listen ready mode=SoftAP ip=192.168.43.1 port=5000`

#### 这一步的正确现象

如果这一步是对的，说明：
- WiFi 子系统已经起来
- 热点已经起来
- 发射板 IP 已固定为 `192.168.43.1`
- TCP 服务器已经监听 `5000`

#### 这一步如果不对

如果看不到上面这几行，就先不要做下一步。

因为这说明问题还在板端初始化，PC 端怎么连都不会成功。

### 3.5 第五步：PC 连接热点

现在在 PC 上搜索 WiFi，连接：

```text
SSID: WS63_LaserTX
Password: ws63laser
```

如果你在手机或电脑上根本搜不到这个热点，先不要怀疑前面的 WiFi 初始化日志。

先看发射板串口这一行：

```text
[wifi gcode] softap ready ssid=WS63_LaserTX ip=192.168.43.1 port=5000 channel=13
```

这里最后的 `channel=13` 很关键。

很多手机、电脑或 USB WiFi 网卡在某些地区设置下，不会显示 `2.4G` 的 `12/13` 信道热点。
所以会出现这种现象：

- 板端日志显示 `softap ready`
- 但手机/电脑 WiFi 列表里看不到 `WS63_LaserTX`

这时候最直接的处理办法不是继续重连，而是把 SoftAP 信道改成 `1`、`6` 或 `11`，优先建议改成 `6`。

修改方法：

1. 执行 `python3 build.py menuconfig ws63-liteos-app`
2. 进入 `Transmitter Configuration`
3. 进入 `WiFi Configuration`
4. 保持 `Support WiFi SoftAP Mode (热点模式)`
5. 把 `Choose SoftAP Channel (信道)` 从 `13` 改成 `6`
6. 保存退出
7. 重新编译并重新下载发射板

重新上电后，你期望看到的日志应该变成：

```text
[wifi gcode] softap ready ssid=WS63_LaserTX ip=192.168.43.1 port=5000 channel=6
[wifi gcode] tcp listen ready mode=SoftAP ip=192.168.43.1 port=5000
```

然后再去手机或电脑上搜：

```text
SSID: WS63_LaserTX
Password: ws63laser
```

连接后，你希望看到的现象是：
- PC 提示“已连接”
- PC 获得 `192.168.43.x` 网段地址

同时发射板串口里通常会看到类似：

```text
[wifi gcode] softap sta join mac=xx:**:**:**:yy:zz count=1
```

这说明确实有设备接入了这个热点。

### 3.6 第六步：验证 TCP 能否连接

在 PC 上执行：

```bash
python3 ws63_test/tools/wifi_bridge.py --host 192.168.43.1 -i
```

正常现象应该类似：

```text
连接 192.168.43.1:5000 ...
  WS63 Laser Marker WiFi
  Grbl 1.1f ['$' for help]
  [MSG:WiFi SoftAP WS63_LaserTX]
  [MSG:Use $WIFI? for link status]
  [MSG:One upstream host at a time]
  WiFi: SoftAP IP=192.168.43.1 SLE=✓

WS63 WiFi Bridge 交互模式
输入 G-Code 命令，按 Enter 发送
特殊命令: /status  /wifi  /quit  /help

ws63>
```

同时板端串口通常会看到：

```text
[wifi gcode] client connected
```

补充说明：
- 当前板端同一时刻只允许一个上游客户端占用会话。
- 如果你已经开了一个 `wifi_bridge.py`、网页控制台或其他 TCP 客户端，第二个客户端再连进来时，新的行为会变成：

```text
  WS63 Laser Marker WiFi
  [MSG:busy another upstream host is connected]
  error:busy
```

- 这不是新故障，而是板端在明确告诉你“当前已有别的上游占着连接”。
- 遇到这种情况时，先退出旧的客户端，再重新连接。

#### 这一步的正确现象

如果这一步对了，说明：
- 热点真的能连
- TCP 5000 端口真的能访问
- 板端欢迎信息已经发出来了

### 3.7 第七步：验证查询类现象

在 `ws63>` 提示符下依次输入：

```text
/wifi
/status
$I
```

#### 输入 `/wifi` 后，预期现象

你应该看到类似：

```text
  [WIFI:MODE=SoftAP,SSID=WS63_LaserTX,IF=ap0,IP=192.168.43.1,NET=1,TCP=1,CLI=1,APSTA=1,STALINK=0,SLE=1,REASON=0]
  模式=SoftAP  IP=192.168.43.1  SLE=✓
```

你重点只看这几项：
- `MODE=SoftAP`
- `IP=192.168.43.1`
- `NET=1`
- `TCP=1`
- `CLI=1`

如果还要验证后级链路：
- `SLE=1`

#### 输入 `/status` 后，预期现象

你应该看到类似：

```text
  <Idle|MPos:0.000,0.000,0.000|FS:0,0>
  状态=Idle  X=0.000  Y=0.000
```

#### 输入 `$I` 后，预期现象

你应该看到类似：

```text
  [VER:1.1f.WS63:]
  [OPT:V,15,128]
  ok
```

### 3.8 第八步：验证最小命令现象

继续输入：

```gcode
G90
G92
M3 S200
G1 X10 Y10 F6000
M5
?
```

预期现象是：

```text
ws63> G90
  ok
ws63> G92
  ok
ws63> M3 S200
  ok
ws63> G1 X10 Y10 F6000
  ok
ws63> M5
  ok
ws63> ?
  <Idle|MPos:10.000,10.000,0.000|FS:0,0>
  状态=Idle  X=10.000  Y=10.000
```

#### 如果你还接着收板和激光

你还应该看到：
- 振镜有动作
- 激光能开能关

### 3.9 SoftAP 成功的判定标准

下面 6 条都满足，就算 `SoftAP` 调通：

1. `menuconfig` 里选的是发射板 + WiFi + SoftAP
2. 编译产出 `ws63-liteos-app_all.fwpkg`
3. 下载后串口出现 `softap ready`
4. 下载后串口出现 `tcp listen ready mode=SoftAP`
5. `wifi_bridge.py --host 192.168.43.1 -i` 能成功连上
6. `/wifi` 里能看到 `MODE=SoftAP, NET=1, TCP=1`

---

## 4. STA 操作步骤

`STA` 一定要在 `SoftAP` 通了以后再做。

因为 `STA` 比 `SoftAP` 多了外部热点和 DHCP 两层变量。

### 4.1 第一步：准备好热点

先准备一个你知道名字和密码的热点：
- 路由器可以
- 手机热点也可以

你要明确知道两件事：
- SSID
- 密码

后面 `menuconfig` 要填进去。

### 4.2 第二步：选择菜单

先执行：

```bash
python3 build.py menuconfig ws63-liteos-app
```

菜单里按下面选。

#### 4.2.1 先选板卡角色

在 `Board Role (板卡角色)` 里：

- 选 `Support Transmitter Board`

最终效果：

```text
CONFIG_LASER_MARKER_TRANSMITTER=y
CONFIG_LASER_MARKER_RECEIVER=n
```

#### 4.2.2 打开 WiFi

进入：

```text
Transmitter Configuration (发射配置)
```

把下面这项打开：

```text
Support WiFi (启用WiFi)
```

也就是：

```text
CONFIG_LASER_WIFI_SOFTAP_ENABLE=y
```

#### 4.2.3 在 WiFi Mode 里选择 STA

进入：

```text
WiFi Configuration (WiFi设置)
```

在 `WiFi Mode (模式)` 里选择：

```text
Support WiFi STA Mode (联网模式)
```

最终要变成：

```text
CONFIG_LASER_WIFI_MODE_SOFTAP=n
CONFIG_LASER_WIFI_MODE_STA=y
```

#### 4.2.4 把你的热点名和密码填进去

把下面两项改成你自己的：

```text
Set STA SSID (路由器名)     = 你的热点名
Set STA Password (路由器密码) = 你的热点密码
Choose TCP Port (端口)       = 5000
```

例如：

```text
CONFIG_LASER_WIFI_STA_SSID="MyHotspot"
CONFIG_LASER_WIFI_STA_PSK="12345678"
CONFIG_LASER_WIFI_TCP_PORT=5000
```

#### 4.2.5 保存并退出

这一步完成后的正确状态是：
- 发射板角色已选对
- WiFi 已开启
- 模式是 `STA`
- SSID 和密码已经改成你的真实热点

### 4.3 第三步：编译

执行：

```bash
python3 build.py ws63-liteos-app
```

如果担心旧配置残留：

```bash
python3 build.py -c ws63-liteos-app
python3 build.py ws63-liteos-app
```

编译成功后，你仍然使用这个固件包：

```text
output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg
```

### 4.4 第四步：下载到发射板

仍然使用你们当前环境里的常用下载方式：
- DevEco Upload
- 或厂商烧录工具

选中的固件文件还是：

```text
output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg
```

如果你还要验证动作闭环，接收板也要保持已烧好接收固件。

### 4.5 第五步：上电后先只看发射板日志

还是先不要跑 PC 脚本，先看发射板调试串口。

正常情况下应该看到类似：

```text
[wifi gcode] wifi init ready
[wifi gcode] wifi event callback ready
[wifi gcode] sta enabled, target ssid=MyHotspot
[wifi gcode] sta scan done
[wifi gcode] sta connected ssid=MyHotspot rssi=-48 channel=6
[wifi gcode] sta ready ssid=MyHotspot ip=192.168.1.123 port=5000
[wifi gcode] tcp listen ready mode=STA ip=192.168.1.123 port=5000
```

你重点只看 3 件事：

1. 有没有 `sta enabled, target ssid=你的热点名`
2. 有没有 `sta ready ssid=你的热点名 ip=实际IP port=5000`
3. 有没有 `tcp listen ready mode=STA ip=实际IP port=5000`

#### 这一步的正确现象

如果这一步对了，说明：
- 发射板已经连上你的热点
- 发射板已经拿到了 DHCP 分配的真实 IP
- TCP 服务器已经在这个真实 IP 上监听

注意：
- 这里的 IP 每次可能不一样
- 后面 PC 连接必须用这里打印出来的真实 IP

### 4.6 第六步：让 PC 接入同一个热点

PC 现在要连接和发射板同一个热点。

例如发射板连的是：

```text
MyHotspot
```

那么你的 PC 也要连：

```text
MyHotspot
```

然后记住刚刚串口里打印的发射板 IP，比如：

```text
192.168.1.123
```

### 4.7 第七步：验证 TCP 能否连接

在 PC 上执行：

```bash
python3 src/ws63_test/tools/wifi_bridge.py --host 192.168.1.123 -i
```

这里的 `192.168.1.123` 必须替换成你日志里的实际 IP。

正常现象应该类似：

```text
连接 192.168.1.123:5000 ...
  WS63 Laser Marker WiFi
  Grbl 1.1f ['$' for help]
  [MSG:WiFi STA MyHotspot]
  [MSG:Use $WIFI? for link status]
  [MSG:One upstream host at a time]
  WiFi: STA IP=192.168.1.123 SLE=✓

WS63 WiFi Bridge 交互模式
输入 G-Code 命令，按 Enter 发送
特殊命令: /status  /wifi  /quit  /help

ws63>
```

同时板端串口通常会看到：

```text
[wifi gcode] client connected
```

补充说明：
- `STA` 模式下也同样是一时刻只允许一个上游客户端占用会话。
- 如果已有别的客户端连着，新客户端会收到：

```text
  WS63 Laser Marker WiFi
  [MSG:busy another upstream host is connected]
  error:busy
```

- 看到这个提示时，不要再怀疑 WiFi 没起，而是先把旧客户端退出。

### 4.8 第八步：验证查询类现象

在 `ws63>` 提示符下依次输入：

```text
/wifi
/status
$I
```

#### 输入 `/wifi` 后，预期现象

你应该看到类似：

```text
  [WIFI:MODE=STA,SSID=MyHotspot,IF=wlan0,IP=192.168.1.123,NET=1,TCP=1,CLI=1,APSTA=0,STALINK=1,SLE=1,REASON=0]
  模式=STA  IP=192.168.1.123  SLE=✓
```

重点只看：
- `MODE=STA`
- `NET=1`
- `TCP=1`
- `CLI=1`
- `STALINK=1`

#### 输入 `/status` 后，预期现象

你应该看到类似：

```text
  <Idle|MPos:0.000,0.000,0.000|FS:0,0>
  状态=Idle  X=0.000  Y=0.000
```

#### 输入 `$I` 后，预期现象

你应该看到类似：

```text
  [VER:1.1f.WS63:]
  [OPT:V,15,128]
  ok
```

### 4.9 第九步：验证最小命令现象

继续输入：

```gcode
G90
G92
M3 S200
G1 X10 Y10 F6000
M5
?
```

预期现象是：

```text
ws63> G90
  ok
ws63> G92
  ok
ws63> M3 S200
  ok
ws63> G1 X10 Y10 F6000
  ok
ws63> M5
  ok
ws63> ?
  <Idle|MPos:10.000,10.000,0.000|FS:0,0>
  状态=Idle  X=10.000  Y=10.000
```

### 4.10 STA 成功的判定标准

下面 6 条都满足，就算 `STA` 调通：

1. `menuconfig` 里选的是发射板 + WiFi + STA
2. 编译产出 `ws63-liteos-app_all.fwpkg`
3. 下载后串口出现 `sta ready ssid=... ip=...`
4. 下载后串口出现 `tcp listen ready mode=STA ip=...`
5. `wifi_bridge.py --host 实际IP -i` 能成功连上
6. `/wifi` 里能看到 `MODE=STA, NET=1, TCP=1, STALINK=1`

---

## 5. 两种模式最简单的验证命令

### 5.1 SoftAP 验证命令

```bash
python3 build.py menuconfig ws63-liteos-app
python3 build.py ws63-liteos-app
python3 src/ws63_test/tools/wifi_bridge.py --host 192.168.43.1 -i
```

### 5.2 STA 验证命令

```bash
python3 build.py menuconfig ws63-liteos-app
python3 build.py ws63-liteos-app
python3 src/ws63_test/tools/wifi_bridge.py --host 启动日志里的实际IP -i
```

---

## 6. 你现在最该怎么做

如果你现在准备开始上板，最推荐的顺序就是：

1. 按第 3 节把 `SoftAP` 菜单选好
2. 编译
3. 下载发射板
4. 只看发射板日志
5. 连 `WS63_LaserTX`
6. 跑 `wifi_bridge.py --host 192.168.43.1 -i`
7. 确认 `/wifi` 里是 `MODE=SoftAP, NET=1, TCP=1`
8. `SoftAP` 通了以后，再按第 4 节切到 `STA`

如果你下一步愿意，我可以继续把这份文档再补成一版“截图式清单”，也就是：
- 菜单要勾哪几项
- 日志里必须出现哪几行
- 哪几行一旦没出现，就说明卡在哪一步  
这样你现场照着勾会更快。 
