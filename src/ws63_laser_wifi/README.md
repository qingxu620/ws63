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

LaserGRBL 可优先尝试 `TCPSerial` 或 `Telnet` 协议连接该 IP/端口。

## 设计边界

- 复用 `ws63_laser_single` 的 G-code、motion、DAC、laser PWM 路径。
- TCP 收到的数据按字节流送入 Grbl 解析，不能把 TCP 包当作一整行。
- `?`、`!`、`~`、`Ctrl-X` 按实时字符处理。
- TCP 断开时强制关光并清 motion queue。

## 当前阶段

这是第一版 SoftAP + TCP Grbl endpoint，用于验证 WiFi 链路是否比 SLE 透明桥更适合实时打标。
暂不加入文件下发、本地缓存执行、多客户端管理等功能。
