# AI 智能创作中枢

`ai_studio` 是 `ws63_test` 的 PC 端 AI 设计上位机。  
它的目标不是替代底层 WS63 固件，而是在现有控制链路之上，补齐一条更适合比赛展示、设计创作和现场演示的完整链路：

`AI 生图 / 本地图导入 -> OpenCV 轮廓提取 -> G-Code 生成 -> 串口或 WiFi 下发到 WS63 发射板`

这份 README 同时承担两种作用：
- 给项目成员快速理解当前阶段成果
- 给后续 AI / 新同学提供“可直接接管”的上下文

---

## 当前阶段结算

当前阶段可以视为 `P2 中期可演示版本`，已经具备以下能力：

- 本地图导入链路已跑通
- 中文路径图片读取已修复
- OpenCV 轮廓提取、降噪、居中排版已接通
- G-Code 生成与导出已接通
- 串口逐行发送并等待 `ok` 的调度链路已接通
- 已支持在 GUI 中选择 `串口 UART` 或 `WiFi TCP` 两种设备通路
- `WiFi TCP` 已细分为 `STA 在线工作流` 与 `SoftAP 离线直连` 两种使用预设
- 连接页已加入 `串口模式 / WiFi SoftAP / WiFi STA` 快捷切换按钮
- 已支持“一键美化提示词”
- 已支持“一键比赛流程”：`AI 生图 -> 轮廓提取 -> G-Code -> 发送`
- GUI 已完成教育类风格重构
- 左侧工作流已改成三步骤标签页
- AI 生图已接入阿里云 `Wan2.7-Image`
- AI 生图结果会自动进入轮廓提取和预览流程

当前版本已经适合做：
- 课堂演示
- 比赛展示
- 功能联调
- 基础打标验证

---

## 目录结构

```text
src/ws63_test/ai_studio/
├── __init__.py
├── README.md
├── qt_compat.py
├── ai_image_generator.py
├── image_processing.py
├── gcode_generator.py
├── serial_worker.py
├── main_window.py
└── main.py
```

---

## 模块职责

### `main.py`
- GUI 启动入口

### `qt_compat.py`
- 统一兼容 `PySide6 / PyQt5`

### `main_window.py`
- 主界面与交互编排中心
- 当前左侧为三步骤标签页：
  - `🔌 步骤一：连接`
  - `🎨 步骤二：创作`
  - `🚀 步骤三：雕刻`
- 已加入“一键美化提示词”与一键比赛流程按钮
- 右侧为：
  - 原图预览
  - 轮廓预览
  - 系统消息板

### `ai_image_generator.py`
- 阿里云 `Wan2.7-Image` REST API 请求
- 子线程 `AIGeneratorThread`
- prompt engineering
- 图片 URL 下载与本地落盘

### `image_processing.py`
- 中文路径图片读取
- 灰度化、自适应阈值、形态学去噪
- Canny + `findContours`
- 轮廓归一化、整体居中、等比例 fit
- 白底黑线轮廓预览图输出

### `gcode_generator.py`
- 将归一化轮廓映射到物理尺寸
- 保持 LaserGRBL 习惯的第一象限正坐标输出
- 按当前板卡幅面限制参数，默认工作区为 `100mm x 100mm`
- 生成 WS63 当前链路兼容的 G-Code
- 生成边框预览 G-Code
- 导出 `.gcode`

### `serial_worker.py`
- `QThread` 统一设备发送线程
- 支持 `串口 UART` 与 `WiFi TCP`
- 逐行发送
- 每行等待下位机 `ok`
- 急停时：
  - 串口模式发送 `M5` 与 `!`
  - WiFi 模式发送 `!`
- 队列调度、进度回传、设备状态管理

---

## 当前真实运行逻辑

### 1. 本地图导入模式
1. 用户在 GUI 中导入图片
2. `image_processing.py` 提取轮廓
3. `gcode_generator.py` 生成 G-Code
4. `serial_worker.py` 通过串口或 WiFi 发送到 WS63 发射板

### 2. AI 生图模式
1. 用户输入 prompt
2. `AIGeneratorThread` 按下拉框选择调用通义万相 2.7 或豆包 Seedream 5.0
3. 生成图片保存为本地文件
4. 自动进入同一条 OpenCV -> G-Code -> 设备发送链路

### 3. 比赛一键流程
1. 用户确认提示词与设备连接方式
2. 点击 `⚡ 一键比赛流程`
3. GUI 自动执行：
   - AI 生图
   - 轮廓提取
   - G-Code 生成
   - 发送到当前选择的 `串口 UART` 或 `WiFi TCP` 设备

---

## AI 生图当前状态

当前使用模型：

- `imagen-4.0-fast-generate-001`

当前提示词策略不是“极端雕刻约束”，而是“展示效果与轮廓提取折中版”。  
附加提示词目前为：

```text
clean high-quality illustration, preserve the character's iconic appearance and canonical colors,
clear black outlines, bright clean white background, high contrast, clear subject, minimal background clutter,
visually appealing, suitable for contour extraction and laser engraving
```

设计意图：
- 保留角色经典配色和展示效果
- 保持背景干净、主体清晰
- 给后续 OpenCV 轮廓提取留出足够对比度

注意：
- 这套提示词仍然是“折中策略”，不是最终最优
- 如果目标偏展示，可继续放松约束
- 如果目标偏雕刻成功率，可增加更硬的线稿约束

---

## 当前 GUI 状态

当前 GUI 已完成以下改造：

- 教育类明亮创客风视觉
- Emoji 情感化标题和按钮
- 三步骤标签页导航
- 一键美化提示词辅助创作
- 一键比赛流程闭环
- 参数控件改成“点击后才能改值”
- 设备未连接时，雕刻动作会被前置拦截

已知 GUI 重点：
- `步骤三：雕刻` 的内容较高，因此已经改成页内滚动思路
- 如果后续 Windows 下仍有个别按钮字体裁切，优先检查：
  - QSS 的 `padding / min-height`
  - Tab 页内部是否为可滚动容器
  - 系统缩放比例是否为 125% / 150%

---

## 当前与底层固件的接口约束

当前上位机默认依赖底层满足以下行为：

- 串口或 WiFi TCP 接收标准 G-Code 文本
- 支持：
  - `G0`
  - `G1`
  - `G90`
  - `M3`
  - `M5`
  - `S`
  - `F`
- 每执行完一行后返回 `ok`

如果底层协议变化，最先需要联动检查的文件是：
- [serial_worker.py](/root/fbb_ws63/src/ws63_test/ai_studio/serial_worker.py)
- [gcode_generator.py](/root/fbb_ws63/src/ws63_test/ai_studio/gcode_generator.py)

---

## 运行方式

### 依赖安装

推荐：

```bash
pip install PySide6 opencv-python pyserial requests numpy
```

兼容：

```bash
pip install PyQt5 opencv-python pyserial requests numpy
```

### 启动

在仓库根目录运行：

```bash
python3 -m src.ws63_test.ai_studio.main
```

---

## 当前关键配置

### AI Key

当前 `Wan2.7-Image` 的 API Key 直接写在：
- [ai_image_generator.py](/root/fbb_ws63/src/ws63_test/ai_studio/ai_image_generator.py)

这在联调阶段方便，但不是最终安全方案。  
后续应迁移到：

- 环境变量 `DASHSCOPE_API_KEY`
- 或单独本地配置文件

### 串口

默认业务串口波特率：
- `115200`

当前设备发送策略：
- 一次只发一行
- 等 `ok` 再发下一行

这是为了稳定，不是为了极限速度。

### 连接方式

比赛版 GUI 中可直接选择：

- `串口 UART`
  - 适合基础联调、和现有板端主基线保持一致
- `WiFi TCP`
  - 适合比赛演示、网页 / 无线接入联调
  - 连接页提供 `串口模式 / WiFi SoftAP / WiFi STA` 快捷切换按钮
  - `STA 在线工作流（推荐）`：适合在线 AI 生图 + WiFi 下发 + 状态展示，填写发射板在路由器或手机热点下获取的实际 IP
  - `SoftAP 离线直连`：适合本地图导入 / 已有 G-Code / 纯控制演示，默认地址通常为 `192.168.43.1:5000`
  - 若当前使用 `SoftAP` 且电脑没有另一条外网链路，在线 AI 生图接口可能无法访问

### 当前板卡坐标约定

- 工作区原点采用 LaserGRBL 常见习惯：`X0 Y0` 位于工作区左下角
- `ai_studio` 输出坐标保持第一象限正数
- 当前默认适配幅面为 `100mm x 100mm`
- 若用户输入尺寸超过该范围，GUI 和 G-Code 生成会直接拒绝，避免板端钳位后图形失真

---

## 已完成问题修复记录

当前已确认修过的关键问题包括：

- Windows 中文路径图片无法读取
- AI 请求阻塞 GUI
- 未连接串口时仍能误下发任务
- 参数框滚轮误触
- 左侧控制区早期的缩放挤压问题
- 轮廓预览图过浅，已改成白底深线

---

## 当前待办 / 下一步建议

如果由新的 AI 或新同学继续接手，优先级建议如下：

### P1：稳定性与可用性
- 统一三个 Tab 页的滚动行为
- 继续修整 Windows 高 DPI 下的按钮裁切问题
- 把 API Key 从代码移到环境变量

### P2：展示效果
- 继续优化“一键美化提示词”的展示效果与打标适配
- 增加生成尺寸、风格参数选择
- 增加 AI 原图的缓存与历史记录

### P3：打标友好优化
- 更好的轮廓简化
- 轮廓排序优化，减少空走
- 更好的预览边界框交互

### P4：平台扩展
- Web 端适配
- 手机端适配
- 后续接入更丰富的创作工作流

---

## 给后续 AI 的接管提示

如果你是后续接管本项目的 AI，请优先理解这几点：

1. 这是一个“上位机前端项目”，不要误把重点放在 WS63 固件里。
2. 当前主目标不是重写架构，而是继续增强：
   - 教育场景体验
   - AI 生图展示效果
   - 打标成功率
3. 当前最容易回归的问题有三类：
   - Windows GUI 高 DPI 下按钮/Tab 裁切
   - 图片路径和保存路径的 Unicode 问题
   - AI 生图提示词过强或过弱导致展示/雕刻失衡
4. 若改 AI 提示词，请同时考虑：
   - 原图展示效果
   - OpenCV 边缘提取效果
   - 激光打标轮廓质量
5. 若改串口逻辑，请不要破坏“逐行等待 ok”这个稳定性前提。

---

## 推荐接管顺序

建议后续 AI 或开发者按以下顺序阅读：

1. [main_window.py](/root/fbb_ws63/src/ws63_test/ai_studio/main_window.py)
2. [ai_image_generator.py](/root/fbb_ws63/src/ws63_test/ai_studio/ai_image_generator.py)
3. [image_processing.py](/root/fbb_ws63/src/ws63_test/ai_studio/image_processing.py)
4. [gcode_generator.py](/root/fbb_ws63/src/ws63_test/ai_studio/gcode_generator.py)
5. [serial_worker.py](/root/fbb_ws63/src/ws63_test/ai_studio/serial_worker.py)

---

## 当前结论

`ai_studio` 已经不是最初的 P0 原型，而是一个：

- 可运行
- 可展示
- 可切换 `串口 UART / WiFi TCP`
- 可本地图导入
- 可 AI 生图
- 可自动生成 G-Code
- 可通过一键美化提示词与一键比赛流程完成现场演示

的阶段性可交付版本。

下一阶段的重点不再是“从 0 到 1 打通链路”，而是：
- 提升展示效果
- 提升界面稳定性
- 提升生成图对打标场景的适配质量
