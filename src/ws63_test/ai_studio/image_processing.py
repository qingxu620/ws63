"""
图像处理与轮廓提取模块。

核心目标：
1. 读取 AI 图像；
2. 提取适合激光轮廓雕刻的路径；
3. 输出归一化路径，方便后续映射到任意实际尺寸。
"""

from __future__ import annotations

from pathlib import Path
from typing import List, Sequence, Tuple

import cv2
import numpy as np

NormalizedPoint = Tuple[float, float]
NormalizedPath = List[NormalizedPoint]


class ImageProcessingError(RuntimeError):
    """图像处理失败时抛出的异常。"""


def _normalize_contour_points(contour: np.ndarray, width: int, height: int) -> NormalizedPath:
    """
    将 OpenCV 轮廓点映射到 0.0~1.0 的归一化坐标。

    说明：
    - X 方向按图像宽度归一化；
    - Y 方向按图像高度归一化；
    - 同时把图像坐标的“左上为原点”转换为雕刻常见的“左下为原点”。
    """

    points: NormalizedPath = []
    for point in contour.reshape(-1, 2):
        px = float(point[0]) / max(width - 1, 1)
        py = 1.0 - (float(point[1]) / max(height - 1, 1))
        points.append((max(0.0, min(1.0, px)), max(0.0, min(1.0, py))))
    return points


def _save_preview_image(image: np.ndarray, output_path: str) -> str:
    output = Path(output_path).resolve()
    cv2.imwrite(str(output), image)
    return str(output)


def process_image_to_contours(
    image_path: str,
    preview_path: str = "temp_contours_preview.png",
    blur_kernel: int = 5,
    canny_low: int = 60,
    canny_high: int = 160,
    min_area: float = 20.0,
) -> Tuple[List[NormalizedPath], str]:
    """
    把输入图像处理成轮廓路径。

    返回：
    - 归一化轮廓点集列表
    - 二值/轮廓预览图路径
    """

    image = cv2.imread(image_path, cv2.IMREAD_COLOR)
    if image is None:
        raise ImageProcessingError(f"无法读取图片: {image_path}")

    gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
    blurred = cv2.GaussianBlur(gray, (blur_kernel, blur_kernel), 0)
    edges = cv2.Canny(blurred, canny_low, canny_high)

    # RETR_TREE 保留内外层结构；CHAIN_APPROX_SIMPLE 可以先做一次基础压缩。
    contours, _ = cv2.findContours(edges, cv2.RETR_TREE, cv2.CHAIN_APPROX_SIMPLE)
    if not contours:
        raise ImageProcessingError("未检测到任何轮廓，请尝试更换图片或调整阈值")

    height, width = gray.shape[:2]
    normalized_paths: List[NormalizedPath] = []
    preview = cv2.cvtColor(edges, cv2.COLOR_GRAY2BGR)

    for contour in contours:
        area = cv2.contourArea(contour)
        if area < min_area:
            continue

        perimeter = cv2.arcLength(contour, True)
        epsilon = 0.0015 * perimeter
        simplified = cv2.approxPolyDP(contour, epsilon, True)
        if len(simplified) < 2:
            continue

        path = _normalize_contour_points(simplified, width, height)
        if len(path) >= 2:
            normalized_paths.append(path)
            cv2.drawContours(preview, [simplified], -1, (0, 0, 255), 1)

    if not normalized_paths:
        raise ImageProcessingError("轮廓过小或过于稀疏，无法生成有效路径")

    preview_file = _save_preview_image(preview, preview_path)
    return normalized_paths, preview_file


def compute_bounding_box(contours: Sequence[NormalizedPath]) -> Tuple[float, float, float, float]:
    """计算归一化轮廓的整体外接矩形。"""

    if not contours:
        raise ImageProcessingError("轮廓为空，无法计算边界")

    xs = [point[0] for path in contours for point in path]
    ys = [point[1] for path in contours for point in path]
    return min(xs), min(ys), max(xs), max(ys)

