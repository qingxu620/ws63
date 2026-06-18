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


def _extract_edges_with_clahe(
    image: np.ndarray,
    *,
    canny_low: int = 30,
    canny_high: int = 100,
    blur_kernel_size: int = 5,
    close_iterations: int = 1,
) -> np.ndarray:
    """
    使用更平衡的 CLAHE 流水线提取边缘。

    处理思路：
    - CLAHE 增强局部对比度，避免主体边缘被吞掉；
    - 轻度高斯模糊抑制细小纹理噪点；
    - 使用更宽容的固定 Canny 阈值保留更多细节；
    - 通过轻微闭运算连接断裂线条，同时尽量不把圆弧挤成粗边。
    """

    gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)

    clahe = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8))
    enhanced = clahe.apply(gray)

    kernel_size = max(3, int(blur_kernel_size))
    if kernel_size % 2 == 0:
        kernel_size += 1
    blurred = cv2.GaussianBlur(enhanced, (kernel_size, kernel_size), 0)
    edged = cv2.Canny(blurred, int(canny_low), int(canny_high))

    kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (3, 3))
    if close_iterations > 0:
        edged = cv2.morphologyEx(edged, cv2.MORPH_CLOSE, kernel, iterations=int(close_iterations))
    return edged


def _smooth_contour_points(
    contour: np.ndarray,
    *,
    is_closed: bool,
    smoothing_passes: int = 2,
) -> np.ndarray:
    """
    对轮廓点做温和平滑，减少像素阶梯感，但尽量保留原始曲线走势。
    """

    points = contour.reshape(-1, 2).astype(np.float32)
    if len(points) < 5:
        return contour.astype(np.float32)

    kernel = np.array([1.0, 4.0, 6.0, 4.0, 1.0], dtype=np.float32)
    kernel /= kernel.sum()

    for _ in range(max(1, smoothing_passes)):
        if is_closed:
            padded = np.vstack([points[-2:], points, points[:2]])
        else:
            padded = np.vstack([points[:1], points[:1], points, points[-1:], points[-1:]])
        smoothed_x = np.convolve(padded[:, 0], kernel, mode="valid")
        smoothed_y = np.convolve(padded[:, 1], kernel, mode="valid")
        points = np.column_stack([smoothed_x, smoothed_y])

    return points.reshape(-1, 1, 2)


def _sample_contour_points_by_distance(
    contour: np.ndarray,
    *,
    is_closed: bool,
    min_distance_px: float = 1.2,
) -> np.ndarray:
    """
    用最小点距做轻量抽样，避免 G-Code 被像素级重复点撑爆。

    这里不使用 approxPolyDP，避免圆弧被近似成明显折线。
    """

    points = contour.reshape(-1, 2).astype(np.float32)
    if len(points) < 3:
        return contour.astype(np.float32)

    sampled = [points[0]]
    for point in points[1:]:
        if np.linalg.norm(point - sampled[-1]) >= min_distance_px:
            sampled.append(point)

    if not is_closed and np.linalg.norm(points[-1] - sampled[-1]) > 1e-6:
        sampled.append(points[-1])
    if is_closed and len(sampled) >= 2 and np.linalg.norm(sampled[0] - sampled[-1]) < min_distance_px * 0.75:
        sampled = sampled[:-1]

    if len(sampled) < 2:
        sampled = [points[0], points[-1]]
    return np.asarray(sampled, dtype=np.float32).reshape(-1, 1, 2)


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
    detail_level: int = 3,
    denoise_level: int = 2,
    smoothing_passes: int = 2,
) -> Tuple[List[NormalizedPath], str]:
    """
    把输入图像处理成轮廓路径。

    当前处理流水线：
    1. 灰度化
    2. CLAHE 增强局部对比度
    3. 轻度高斯模糊
    4. 宽容 Canny
    5. 轻微闭运算连接断线
    6. 轮廓提取
    7. 过滤极小噪点并做高密度平滑
    8. 居中排版并输出预览图

    返回：
    - 归一化轮廓点集列表
    - 白底深线轮廓预览图路径
    """

    image = _read_image_unicode_safe(image_path)

    detail = max(1, min(5, int(detail_level)))
    denoise = max(0, min(5, int(denoise_level)))
    canny_low = max(10, 45 - detail * 5)
    canny_high = max(canny_low + 30, 130 - detail * 10)
    blur_kernel_size = 3 if denoise <= 1 else 5 if denoise <= 3 else 7
    close_iterations = min(3, denoise)
    sample_distance_px = max(0.6, 1.8 - detail * 0.2)

    edged = _extract_edges_with_clahe(
        image,
        canny_low=canny_low,
        canny_high=canny_high,
        blur_kernel_size=blur_kernel_size,
        close_iterations=close_iterations,
    )

    contours, _ = cv2.findContours(edged, cv2.RETR_LIST, cv2.CHAIN_APPROX_NONE)
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

        is_closed = area >= 1.0
        smoothed = _smooth_contour_points(
            contour,
            is_closed=is_closed,
            smoothing_passes=max(1, int(smoothing_passes)),
        )
        sampled = _sample_contour_points_by_distance(
            smoothed,
            is_closed=is_closed,
            min_distance_px=sample_distance_px,
        )
        if len(sampled) < 2:
            continue

        path = _normalize_contour_points(sampled, width, height)
        if len(path) < 2:
            continue

        raw_paths.append(path)
        preview_points = np.round(sampled).astype(np.int32)
        cv2.polylines(preview, [preview_points], isClosed=is_closed, color=(32, 32, 32), thickness=2)

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
