# WS63 Host 与 LaserGRBL 图像转 G-code 对标矩阵

更新日期：2026-07-21  
LaserGRBL 对照基线：v7.14.1；同时复核官方仓库提交 `1f9337b3af27133f8b1696e41cc110f2af74d04f`（相关图像转换文件与 v7.14.1 无差异）。

本矩阵比较图像预处理、路径提取和 G-code 生成能力，不要求两端输出逐字节一致。状态定义：

- ✅ 已实现：已有可用的用户入口和核心处理链路；允许协议适配层面的差异。
- 🟡 部分实现：目标能力存在，但算法、参数范围、路径语义或格式覆盖尚未对齐。
- ❌ 未实现：当前没有对应入口或核心算法。

> **术语边界**：当前 `lasergrbl_vector` 是“LaserGRBL 风格独立轮廓提取”，由阈值分割、连通域清理、网格边界跟踪、RDP 简化和 Chaikin 平滑组成，**不是原生 Potrace，也没有集成 CsPotrace**。文档与界面不应将其称为“原生 Potrace”或未经验证的“Potrace 兼容实现”。当前中心线同样不是 Potrace，而是 Zhang-Suen 细化；LaserGRBL 的中心线后端则是 AutoTrace。

## 功能矩阵

| 能力 | LaserGRBL 参考行为 | 当前状态 | WS63 Host 现状与主要差距 | 官方来源 |
|---|---|:---:|---|---|
| 集成式导入与实时预览 | 导入窗口集中展示参数、原图和转换预览 | ✅ | `图像参数...` 同一窗口内包含模式、参数、预览和裁剪；预览使用固定显示分辨率、150 ms 合并刷新和单线程后台任务，质量参数只更新轨迹估算 | [Raster import][lg-raster]、[Import parameters][lg-params] |
| 图片格式 | JPG/JPEG、BMP、PNG、GIF | ✅ | 显式筛选 JPG/JPEG、BMP、PNG、GIF，并额外支持 WebP。见 [gcode_page.py](../ui/pages/gcode_page.py) | [Raster import][lg-raster] |
| 手动裁剪 | 鼠标选择保留区域 | ✅ | 原参数窗口内可拖动裁剪，选区按图像边界钳制。见 [crop_image_widget.py](../ui/widgets/crop_image_widget.py) | [Import parameters][lg-params] |
| 自动去边 | 按边缘背景色与容差自动裁边 | ✅ | “自动裁剪主体”从四周稳健采样背景色，支持白色、灰色、彩色和透明背景，过滤孤立噪点并保留封闭背景孔洞。见 [image_crop.py](../app/image_crop.py) | [ImageProcessor.cs][lg-imageprocessor] |
| 旋转、翻转、反色、恢复 | 90° 旋转、水平/垂直翻转、反色、恢复原图 | ✅ | 均在原参数窗口内提供，并可与裁剪统一确认 | [Import parameters][lg-params] |
| 区域填黑/填白与 Outliner | 点击区域洪泛填充；Outliner 将背景与主体分离 | ❌ | 当前没有取样点洪泛填充或 Outliner 交互 | [ImageProcessor.cs][lg-imageprocessor] |
| 缩放插值 | NearestNeighbor 与 HighQualityBicubic | 🟡 | 已有 NearestNeighbor 与 Qt Smooth/Bilinear；尚非 HighQualityBicubic | [ImageTransform.cs][lg-imagetransform] |
| 灰度公式 | Simple Average、Weighted Average、Optical Correct、自定义 RGB 权重 | ✅ | 四种公式及自定义 RGB 权重已实现，透明像素先合成到白底。见 [image_gcode.py](../app/image_gcode.py) | [Import parameters][lg-params]、[ImageTransform.cs][lg-imagetransform] |
| 亮度、对比度、白场与阈值 | 亮度/对比度、White Clip、可选黑白阈值 | ✅ | 亮度/对比度采用 0–200、默认 100 的线性颜色矩阵；White Clip 采用默认 5 的近白清除语义；阈值以 8-bit 数值显示，并额外提供黑场增强 | [Import parameters][lg-params]、[ImageTransform.cs][lg-imagetransform] |
| 线到线灰度 PWM | 8-bit 灰度映射到 S 范围，合并相同功率像素，白区快速移动 | ✅ | 已有 S-min/S-max、同功率游程合并、白区 `G0`/`G1` 策略、M3/M4，并采用模态轴与行内 `S` 的紧凑色段输出；纯白任务不生成机械移动。见 [image_gcode.py](../app/image_gcode.py) | [Line-to-line][lg-line]、[GrblFile.cs][lg-grblfile] |
| 黑白线稿文件精简 | 黑白阈值后的色段数量远少于带微小灰度噪声的 PWM 色段 | ✅ | 默认自动识别高对比黑白线稿并使用二值色段，避免近白背景和近黑笔画中的不可见噪声把文件放大数十倍；用户可关闭并强制保留完整 8-bit 灰度 PWM | [Line-to-line][lg-line]、[GrblFile.cs][lg-grblfile] |
| 扫描方向与线密度 | 水平/垂直/对角、双向/单向；常规最高 20 线/mm，高分辨率模式最高 50 线/mm | ✅ | 已有三方向、双向/单向、预计轨迹数和 1–50 线/mm | [Line-to-line][lg-line]、[GrblFile.cs][lg-grblfile] |
| 1-bit 抖动算法 | Atkinson、Floyd-Steinberg、Burkes、Jarvis-Judice-Ninke、Random、Sierra 2/3/Lite、Stucki | 🟡 | 九种选择均有，误差扩散默认与 LaserGRBL 一样逐行同向；Random 使用固定种子而非时钟种子，以保证预览与最终 G-code 一致 | [Dithering][lg-dither]、[ImageTransform.cs][lg-imagetransform] |
| 位图轮廓提取工作流 | 二值图经 Potrace 生成轮廓 | 🟡 | 已有可用的独立轮廓提取，但后端不是 Potrace；复杂孔洞、拐角策略与曲线拟合结果可能不同。见 [image_gcode.py](../app/image_gcode.py) | [Vectorization][lg-vector]、[Potrace manual][potrace-manual] |
| 原生 Potrace 参数与曲线模型 | `turdsize`、`alphamax`、`opticurve`、`opttolerance`、`turnpolicy` | ❌ | 当前“斑点清除/光滑/优化”是连通域、RDP/Chaikin 与行程排序，不能与这些 Potrace 参数一一等同 | [Vectorization][lg-vector]、[Potrace manual][potrace-manual] |
| 矢量曲线 G-code | Potrace Bézier 经双圆弧近似输出 G2/G3，失败时退回 G1 | ❌ | 当前轮廓只输出 G1 折线；Chaikin 仅增加平滑采样点，不会生成圆弧 | [GrblFile.cs][lg-grblfile] |
| 中心线 | 外部 AutoTrace `-centerline` 后端，64 位可用 | 🟡 | 已有 Zhang-Suen 细化与 8 连通路径跟踪，可生成连续 G-code；算法和参数语义不等同 AutoTrace | [Vectorization][lg-vector]、[AutoTrace wrapper][lg-autotrace] |
| 矢量填充 | 传统水平/垂直/对角扫描及多种 Clipper 图案：网格、交叉、方格、Zigzag、Hilbert、Inset 等 | 🟡 | 已有无填充、水平、垂直、水平+垂直交叉、45° 对角；本质仍是掩膜栅格扫描，尚缺反向对角及高级图案/内缩填充 | [Vectorization][lg-vector]、[PotraceClipper.cs][lg-clipper] |
| 行程排序 | 贪心最近邻优化；填充路径分块处理 | 🟡 | 轮廓和中心线已有贪心最近邻；尚未对齐内外轮廓顺序、路径反转策略及大规模填充分块规则 | [GrblFile.cs][lg-grblfile]、[PotraceClipper.cs][lg-clipper] |
| 缩减取样与自适应矢量质量 | 下采样 1–10；可按图形面积自适应输入分辨率 | 🟡 | 已有 1–4 倍下采样、自适应质量开关和 2–40 px/mm，但公式与范围不同 | [Vectorization][lg-vector]、[GrblFile.cs][lg-grblfile] |
| 目标与激光参数 | 宽高、质量、位置、速度、S 范围等 | ✅ | 已有宽高、比例锁定、XY 偏移、DPI 自动尺寸、M3/M4、轮廓/填充速度和 S-min/S-max。见 [gcode_page.py](../ui/pages/gcode_page.py) | [Target size and laser options][lg-target] |
| 高质量转换响应性 | 图像导入与 G-code 加载在工作线程执行，界面保持可操作 | ✅ | 高密度栅格转换在独立线程执行，可即时取消；最终效果图限制为显示分辨率，不改变生成质量；超过 512 KiB 的编辑器内容关闭装饰性语法高亮，避免逐行高亮阻塞主线程 | [GrblFile.cs][lg-grblfile] |
| 生成文件大小边界 | 生成阶段不按 G-code 字节数截断；高质量大图可产生数百万行 | ✅ | 转换阶段完整生成，只显示字节数，不按 100 KiB 拒绝或警告。100 KiB 审核仅属于“仅上传”传输边界；“上传并执行”走预加载/流式策略。见 [sle_tx_transport.py](../transports/sle_tx_transport.py) 与 [main_window.py](../ui/main_window.py) | [LaserGRBL FAQ][lg-faq] |

## 当前实现中超出 LaserGRBL 基线的能力

- `轮廓矢量（边缘增强）`提供局部均值自适应掩膜和边缘增强；LaserGRBL 的官方导入器没有对应高级滤镜。
- 黑场增强是 WS63 Host 扩展；LaserGRBL 对应流程主要使用 White Clip。
- Random 抖动固定种子，使预览、测试和最终 G-code 可复现；这与 LaserGRBL 的时钟种子行为有意不同。
- 在 LaserGRBL 所列格式之外显式支持 WebP。

这些扩展应作为本项目能力保留，但不能用来证明与 LaserGRBL 像素级一致。LaserGRBL 本身也未提供 gamma、曲线、直方图均衡、Gaussian/median/bilateral、锐化、形态学等完整照片编辑滤镜组；这些若新增，应归类为产品增强而非 LaserGRBL 对标缺口。

## 建议收敛顺序

1. **P0：统一术语与金样测试。** 将用户可见的“Potrace 风格/兼容”收敛为“LaserGRBL 风格独立轮廓（非 Potrace）”；用固定输入图对灰度、九种抖动、线密度、孔洞轮廓和 G-code 边界做回归。
2. **P1：补齐高频栅格差异。** 可继续增加“蛇形误差扩散”扩展开关；默认同向算法保持 LaserGRBL 行为。
3. **P1：扩展填充。** 优先补反向对角、对角交叉、网格与 Inset；Hilbert/Zigzag/方格可后置。
4. **P2：明确矢量后端路线。** 若追求算法级一致，应通过独立、可替换的真实 Potrace 后端实现并完成 GPL 许可评审；否则继续优化自研轮廓质量，但始终保持“非 Potrace”标识。
5. **P2：补交互型预处理。** 增加区域填黑/填白、Outliner、GIF 导入及真正的 Bicubic 缩放。

## 代码证据入口

- 图像算法：[app/image_gcode.py](../app/image_gcode.py)
- 裁剪与几何处理：[app/image_crop.py](../app/image_crop.py)
- 参数窗口及转换调度：[ui/pages/gcode_page.py](../ui/pages/gcode_page.py)
- 交互裁剪控件：[ui/widgets/crop_image_widget.py](../ui/widgets/crop_image_widget.py)
- 算法回归：[tests/test_image_gcode.py](../tests/test_image_gcode.py)、[tests/test_image_crop.py](../tests/test_image_crop.py)、[tests/test_ui_contract.py](../tests/test_ui_contract.py)
- 第三方边界：[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)

[lg-raster]: https://lasergrbl.com/usage/raster-image-import/
[lg-params]: https://lasergrbl.com/usage/raster-image-import/import-parameters/
[lg-line]: https://lasergrbl.com/usage/raster-image-import/line-to-line-tool/
[lg-dither]: https://lasergrbl.com/usage/raster-image-import/dithering-tool/
[lg-vector]: https://lasergrbl.com/usage/raster-image-import/vectorization-tool/
[lg-target]: https://lasergrbl.com/usage/raster-image-import/target-image-size-and-laser-options/
[lg-faq]: https://lasergrbl.com/faq/
[potrace-manual]: https://potrace.sourceforge.net/potrace.1.html
[lg-imageprocessor]: https://github.com/arkypita/LaserGRBL/blob/1f9337b3af27133f8b1696e41cc110f2af74d04f/LaserGRBL/RasterConverter/ImageProcessor.cs
[lg-imagetransform]: https://github.com/arkypita/LaserGRBL/blob/1f9337b3af27133f8b1696e41cc110f2af74d04f/LaserGRBL/RasterConverter/ImageTransform.cs
[lg-grblfile]: https://github.com/arkypita/LaserGRBL/blob/1f9337b3af27133f8b1696e41cc110f2af74d04f/LaserGRBL/GrblFile.cs
[lg-clipper]: https://github.com/arkypita/LaserGRBL/blob/1f9337b3af27133f8b1696e41cc110f2af74d04f/LaserGRBL/CsPotrace/PotraceClipper.cs
[lg-autotrace]: https://github.com/arkypita/LaserGRBL/blob/1f9337b3af27133f8b1696e41cc110f2af74d04f/LaserGRBL/Autotrace/Autotrace.cs
