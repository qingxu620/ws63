# WS63 Laser Host UI

这是 WS63 激光打标系统的正式 PySide6 上位机工程骨架。

第一阶段只实现 UI 骨架和三模连接框架，不生成 G-code，不下发真实 job packet。

## 支持模式

- USART Direct: PC -> RX
- SLE via TX: PC -> TX -> SLE -> RX
- WiFi TCP: PC -> RX WiFi Server

## 运行环境

Windows 11 上运行，推荐使用 Python venv。

```cmd
cd /d C:\Users\ZKX\OneDrive\Desktop\ws63_laser_host_ui
python -m venv .venv
.venv\Scripts\activate
pip install -r requirements.txt
python main.py
```

WSL2 中也可以做语法检查和源码维护，但不要假设 WSL2 能直接访问 Win11 COM 口。

## 当前功能

- PySide6 主窗口。
- 左侧导航：
  - Connection
  - Import
  - G-code Preview
  - Job
  - Monitor
  - Logs
  - Settings
- 顶部状态栏：
  - 当前模式
  - RX 状态
  - TX 状态
  - SLE 状态
  - Laser 状态
- Connection 页面：
  - USART Direct
  - SLE via TX
  - WiFi TCP
- Logs 页面：
  - host / tx_log / rx_log / status / error / job 分类
  - 清空日志
  - 暂停自动滚动
- Settings 页面：
  - 默认连接模式
  - 串口角色映射
  - WiFi IP/Port
  - JSON 配置保存

## 配置文件

配置默认保存到：

```text
config/host_ui_config.json
```

不要在源码里写死 COM 口。每次 demo 前确认 Win11 当前串口角色映射。

## 后续阶段

1. 接入真实 USART / WiFi 基础命令。
2. 接入 SLE TX 命令串口和 TX/RX 日志监听。
3. 接入 G-code 文件导入、预览和 job 下发。
4. 增加图像矢量化、路径规划和打标参数面板。
5. 做比赛展示版主题和设备状态仪表盘。
