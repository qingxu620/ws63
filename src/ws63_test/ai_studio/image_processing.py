"""
图像处理与轮廓提取模块。

核心目标：
1. 读取 AI 图像或本地图像；
2. 从复杂照片、插画或 3D 渲染图中尽量提取干净、连续、平滑的轮廓；
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


def _save_preview_image(image: np.ndarray, output_path: str) -> str:
    """以 Unicode 安全方式保存轮廓预览图。"""

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


def _normalize_contour_points(contour: np.ndarray, width: int, height: int) -> NormalizedPath:
    """
    将 OpenCV 轮廓点映射到 0.0~1.0 的归一化坐标。

    同时把图像坐标的“左上为原点”转换为雕刻常见的“左下为原点”。
    """

    points: NormalizedPath = []
    for point in contour.reshape(-1, 2):
        px = float(point[0]) / max(width - 1, 1)
        py = 1.0 - (float(point[1]) / max(height - 1, 1))
        points.append((max(0.0, min(1.0, px)), max(0.0, min(1.0, py))))
    return points


def _extract_edges_with_clahe(image: np.ndarray) -> np.ndarray:
    """
    使用更平衡的 CLAHE 流水线提取边缘。

    处理思路：
    - CLAHE 增强局部对比度，避免主体边缘被吞掉；
    - 轻度高斯模糊抑制细小纹理噪点；
    - 使用更宽容的固定 Canny 阈值保留更多细节；
    - 轻度膨胀连接断裂线条。
    """

    gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)

    clahe = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8))
    enhanced = clahe.apply(gray)

    blurred = cv2.GaussianBlur(enhanced, (5, 5), 0)
    edged = cv2.Canny(blurred, 40, 120)

    kernel = np.ones((2, 2), np.uint8)
    edged = cv2.dilate(edged, kernel, iterations=1)
    return edged


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
    min_area: float = 20.0,
    min_perimeter: float = 10.0,
) -> Tuple[List[NormalizedPath], str]:
    """
    把输入图像处理成轮廓路径。

    当前处理流水线：
    1. 灰度化
    2. CLAHE 增强局部对比度
    3. 轻度高斯模糊
    4. 宽容 Canny
    5. 轻度膨胀连接断线
    6. 轮廓提取
    7. 过滤极小噪点并平滑
    8. 居中排版并输出预览图

    返回：
    - 归一化轮廓点集列表
    - 白底深线轮廓预览图路径
    """

    image = _read_image_unicode_safe(image_path)

    edged = _extract_edges_with_clahe(image)

    contours, _ = cv2.findContours(edged, cv2.RETR_LIST, cv2.CHAIN_APPROX_SIMPLE)
    if not contours:
        raise ImageProcessingError("未检测到任何轮廓，请尝试更换图片")

    height, width = image.shape[:2]
    preview = np.full((height, width, 3), 255, dtype=np.uint8)
    raw_paths: List[NormalizedPath] = []

    for contour in contours:
        area = cv2.contourArea(contour)
        perimeter = cv2.arcLength(contour, True)
        if perimeter < min_perimeter:
            continue
        if area < min_area and perimeter < (min_perimeter * 2.0):
            continue

        epsilon = 0.005 * perimeter
        approx = cv2.approxPolyDP(contour, epsilon, False)
        if len(approx) < 2:
            continue

        path = _normalize_contour_points(approx, width, height)
        if len(path) < 2:
            continue

        raw_paths.append(path)
        is_closed = area >= 1.0
        cv2.polylines(preview, [approx], isClosed=is_closed, color=(32, 32, 32), thickness=2)

    if not raw_paths:
        raise ImageProcessingError("轮廓过小或噪点过多，无法生成有效路径")

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
