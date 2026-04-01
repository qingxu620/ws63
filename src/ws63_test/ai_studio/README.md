# AI 智能创作中枢

这是 `ws63_test` 的 PC 端上位机原型，用于把“AI 生图 -> 图像轮廓提取 -> G-Code 生成 -> 串口下发到 WS63 发射板”串成一条最小闭环。

## 目录结构建议

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

## 模块说明

- `ai_image_generator.py`
  AI 生图接口。默认兼容 OpenAI 风格图片生成接口，未配置 API 时自动走 mock 图。
- `image_processing.py`
  基于 OpenCV 的轮廓提取模块，输出归一化路径。
- `gcode_generator.py`
  基于轮廓生成 WS63 当前可执行的 G-Code，并支持边界框预览。
- `serial_worker.py`
  基于 `pyserial + QThread` 的串口发送线程，逐行等待 `ok`。
- `main_window.py`
  主界面，负责把 AI、图像处理、G-Code 和串口调度串起来。
- `main.py`
  启动入口。

## 依赖安装

建议使用独立虚拟环境：

```bash
pip install PySide6 opencv-python pyserial requests numpy
```

如果你更习惯 `PyQt5`，也可以安装：

```bash
pip install PyQt5 opencv-python pyserial requests numpy
```

## 启动方式

在仓库根目录执行：

```bash
python3 -m src.ws63_test.ai_studio.main
```

如果你进入 `src/ws63_test/ai_studio` 目录，也可以直接运行：

```bash
python3 main.py
```

## AI 接口配置

当前默认读取以下环境变量：

- `OPENAI_BASE_URL`
- `OPENAI_API_KEY`
- `OPENAI_IMAGE_MODEL`

未配置时，不会报错退出，而是自动生成一张 mock 示例图，方便先联调下位机和轨迹链路。

## 当前阶段定位

这版更偏 P0 原型，目标是尽快把链路跑通：

1. 输入提示词
2. 生成图片
3. 提取轮廓
4. 生成 G-Code
5. 导出给 LaserGRBL，或直接串口发给 WS63 发射板

后续可继续扩展：

- 轨迹缩放、平移、旋转和居中
- 图片上传而不仅是 AI 生图
- 路径简化和 G2/G3 圆弧拟合
- 灰度雕刻 / 光栅雕刻
- Web 端与移动端适配

