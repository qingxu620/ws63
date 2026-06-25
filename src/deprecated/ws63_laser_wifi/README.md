# WS63 Laser WiFi

单板 WiFi 实时打标主线实验工程。

## 架构

```text
PC / LaserGRBL / LightBurn
    -> WiFi SoftAP
    -> TCP socket
    -> WS63 RX/controller
    -> Grbl parser / motion executor / DAC / laser PWM
```

这个工程没有发射板，WS63 接收板就是唯一的 Grbl 设备。

## 默认连接参数

- SSID: `WS63_LASER_WIFI`
- Password: `12345678`
- Controller IP: `192.168.43.1`
- TCP port: `5000`
- Channel: `13`

LaserGRBL 使用 `Telnet` 协议连接该 IP/端口。若你的版本提供其它普通 TCP 客户端协议，也可以作为备选验证。

## LaserGRBL 调试流程

1. 烧录 `ws63_laser_wifi` 到接收板。
2. 电脑连接热点 `WS63_LASER_WIFI`，密码 `12345678`。
3. LaserGRBL 打开 `Grbl -> 设置 -> 协议`。
4. 连接协议选择 `Telnet`，地址填 `192.168.43.1`，端口填 `5000`。
5. 如果其它普通 TCP 客户端协议可用，也可以作为备选验证；不要选择 WebSocket 专用协议。
6. 首次连接后，在 COM26 debug 口确认出现：

```text
[laser wifi] wifi event callback ready
[laser wifi] softap state=... available
[laser wifi] softap ssid=WS63_LASER_WIFI ip=192.168.43.1 port=5000 channel=13 hidden_flag=1
[laser wifi] tcp server listening port=5000
[laser wifi] client connected sock=...
[WIFI_TCP_RX] ...
[WIFI_LINE] id=... line="..."
[WIFI_PARSE] id=... cmds=...
[WIFI_OK] ...
```

如果 LaserGRBL 卡住，先不要改代码，抓 COM26 日志判断：

- 有 `[WIFI_TCP_RX]` 但没有 `[WIFI_LINE]`：行结束符或字节流拼接问题。
- 有 `[WIFI_LINE]` 但没有 `[WIFI_PARSE]`：命令被 `$`/实时命令分支处理，或格式不符合普通 G-code。
- 有 `[WIFI_PARSE]` 但没有 `[WIFI_OK]`：运动队列或 M5 drain 阻塞。
- 有 `[WIFI_OK]` 但 LaserGRBL 仍卡：TCP 回包或 LaserGRBL 网络协议解析问题。

## 设计边界

- 复用 `ws63_laser_single` 的 G-code、motion、DAC、laser PWM 路径。
- TCP 收到的数据按字节流送入 Grbl 解析，不能把 TCP 包当作一整行。
- `?`、`!`、`~`、`Ctrl-X` 按实时字符处理。
- TCP 断开时强制关光并清 motion queue。

## 当前阶段

这是第一版 SoftAP + TCP Grbl endpoint，用于验证 WiFi 链路是否比 SLE 透明桥更适合实时打标。
暂不加入文件下发、本地缓存执行、多客户端管理等功能。
