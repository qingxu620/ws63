"""
图像处理与轮廓提取模块。

核心目标：
1. 读取 AI 图像；
2. 提取适合激光轮廓雕刻的路径；
3. 输出归一化且居中排版后的路径，方便后续映射到任意实际尺寸。
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


def _read_image_unicode_safe(image_path: str) -> np.ndarray:
    """
    读取支持中文路径的图片。

    说明：
    - Windows 下 `cv2.imread()` 对 Unicode 路径支持不稳定；
    - 这里改用 `np.fromfile() + cv2.imdecode()`，兼容本地中文目录与文件名。
    """

    try:
        image_data = np.fromfile(image_path, dtype=np.uint8)
    except OSError as exc:
        raise ImageProcessingError(f"无法读取图片文件: {image_path}") from exc

    if image_data.size == 0:
        raise ImageProcessingError(f"图片文件为空或不可访问: {image_path}")

    image = cv2.imdecode(image_data, cv2.IMREAD_COLOR)
    if image is None:
        raise ImageProcessingError(f"无法解码图片文件: {image_path}")
    return image


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
    suffix = output.suffix or ".png"
    success, encoded = cv2.imencode(suffix, image)
    if not success or encoded is None:
        raise ImageProcessingError(f"无法编码预览图片: {output}")
    try:
        encoded.tofile(str(output))
    except OSError as exc:
        raise ImageProcessingError(f"无法保存预览图片: {output}") from exc
    return str(output)


def _denoise_binary_mask(gray: np.ndarray) -> np.ndarray:
    """
    先做自适应阈值，再做形态学开闭运算。

    这样能显著削弱 AI 图像里常见的背景颗粒、碎边和细小噪点。
    """

    binary = cv2.adaptiveThreshold(
        gray,
        255,
        cv2.ADAPTIVE_THRESH_GAUSSIAN_C,
        cv2.THRESH_BINARY_INV,
        31,
        8,
    )
    kernel_small = cv2.getStructuringElement(cv2.MORPH_RECT, (3, 3))
    kernel_large = cv2.getStructuringElement(cv2.MORPH_RECT, (5, 5))
    opened = cv2.morphologyEx(binary, cv2.MORPH_OPEN, kernel_small, iterations=1)
    closed = cv2.morphologyEx(opened, cv2.MORPH_CLOSE, kernel_large, iterations=1)
    return closed


def _center_and_fit_contours(
    contours: Sequence[NormalizedPath],
    margin_ratio: float = 0.08,
) -> List[NormalizedPath]:
    """
    将全部轮廓整体等比例缩放并平移到 0~1 画布中央。

    这样后续无论用户设置 50x50 还是 80x50 mm，
    都能保持图形比例不变，同时尽量居中，不会贴边或溢出。
    """

    if not contours:
        raise ImageProcessingError("轮廓为空，无法执行居中排版")

    min_x, min_y, max_x, max_y = compute_bounding_box(contours)
    src_w = max(max_x - min_x, 1e-6)
    src_h = max(max_y - min_y, 1e-6)
    usable = max(0.1, 1.0 - 2.0 * margin_ratio)
    scale = min(usable / src_w, usable / src_h)

    dst_w = src_w * scale
    dst_h = src_h * scale
    offset_x = (1.0 - dst_w) * 0.5
    offset_y = (1.0 - dst_h) * 0.5

    result: List[NormalizedPath] = []
    for path in contours:
        transformed: NormalizedPath = []
        for x, y in path:
            tx = (x - min_x) * scale + offset_x
            ty = (y - min_y) * scale + offset_y
            transformed.append((max(0.0, min(1.0, tx)), max(0.0, min(1.0, ty))))
        result.append(transformed)
    return result


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

    image = _read_image_unicode_safe(image_path)

    gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
    denoised_mask = _denoise_binary_mask(gray)
    blurred = cv2.GaussianBlur(denoised_mask, (blur_kernel, blur_kernel), 0)
    edges = cv2.Canny(blurred, canny_low, canny_high)

    # RETR_TREE 保留内外层结构；CHAIN_APPROX_SIMPLE 可以先做一次基础压缩。
    contours, _ = cv2.findContours(edges, cv2.RETR_TREE, cv2.CHAIN_APPROX_SIMPLE)
    if not contours:
        raise ImageProcessingError("未检测到任何轮廓，请尝试更换图片或调整阈值")

    height, width = gray.shape[:2]
    raw_paths: List[NormalizedPath] = []
    # 预览图使用白底黑线，更符合教育软件和草图式视觉风格。
    preview = np.full((height, width, 3), 255, dtype=np.uint8)

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
            raw_paths.append(path)
            # 预览图适当加粗轮廓线，避免在白底和缩略显示时显得过浅。
            cv2.drawContours(preview, [simplified], -1, (32, 32, 32), 2)

    if not raw_paths:
        raise ImageProcessingError("轮廓过小或过于稀疏，无法生成有效路径")

    normalized_paths = _center_and_fit_contours(raw_paths)
    preview_file = _save_preview_image(preview, preview_path)
    return normalized_paths, preview_file


def compute_bounding_box(contours: Sequence[NormalizedPath]) -> Tuple[float, float, float, float]:
    """计算归一化轮廓的整体外接矩形。"""

    if not contours:
        raise ImageProcessingError("轮廓为空，无法计算边界")

    xs = [point[0] for path in contours for point in path]
    ys = [point[1] for path in contours for point in path]
    return min(xs), min(ys), max(xs), max(ys)
