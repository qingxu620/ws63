"""Image cropping helpers for the Host image workspace."""
from __future__ import annotations

from collections import deque
from dataclasses import dataclass
import math

from PySide6.QtCore import QRect, QSize, Qt
from PySide6.QtGui import QImage, QTransform


AUTO_CROP_ANALYSIS_MAX_PX = 512


@dataclass(frozen=True)
class AutoCropResult:
    """Result of removing border-connected background."""

    image: QImage
    crop_rect: QRect
    source_size: QSize

    @property
    def cropped(self) -> bool:
        return self.crop_rect != QRect(
            0, 0, self.source_size.width(), self.source_size.height()
        )


def rotate_image_quarter_turns(image: QImage, quarter_turns: int) -> QImage:
    """Rotate a QImage clockwise in 90-degree increments."""

    if image.isNull():
        raise ValueError("image must be non-null")
    turns = int(quarter_turns) % 4
    if turns == 0:
        return image.copy()
    transform = QTransform()
    transform.rotate(90 * turns)
    return image.transformed(transform, Qt.TransformationMode.SmoothTransformation)


def flip_image(
    image: QImage,
    *,
    horizontal: bool = False,
    vertical: bool = False,
) -> QImage:
    """Mirror a QImage along either or both axes."""

    if image.isNull():
        raise ValueError("image must be non-null")
    if not horizontal and not vertical:
        return image.copy()
    orientation = (
        Qt.Orientation.Horizontal
        if horizontal
        else Qt.Orientation.Vertical
    )
    if horizontal and vertical:
        orientation = Qt.Orientation.Horizontal | Qt.Orientation.Vertical
    flipped = getattr(image, "flipped", None)
    if callable(flipped):
        return flipped(orientation)
    return image.mirrored(bool(horizontal), bool(vertical))


def invert_image_colors(image: QImage) -> QImage:
    """Invert RGB channels while preserving alpha."""

    if image.isNull():
        raise ValueError("image must be non-null")
    result = image.convertToFormat(QImage.Format.Format_ARGB32)
    result.invertPixels(QImage.InvertMode.InvertRgb)
    return result


def _composited_rgb(data: memoryview, index: int) -> tuple[int, int, int]:
    """Read RGBA8888 and composite transparent pixels over white."""

    red = int(data[index])
    green = int(data[index + 1])
    blue = int(data[index + 2])
    alpha = int(data[index + 3])
    if alpha >= 255:
        return red, green, blue
    inverse = 255 - alpha
    return (
        (red * alpha + 255 * inverse + 127) // 255,
        (green * alpha + 255 * inverse + 127) // 255,
        (blue * alpha + 255 * inverse + 127) // 255,
    )


def _median(values: list[int]) -> int:
    ordered = sorted(values)
    return ordered[len(ordered) // 2]


def auto_crop_white_background(
    image: QImage,
    *,
    tolerance: int = 18,
    padding_ratio: float = 0.02,
    max_padding_px: int = 16,
) -> AutoCropResult:
    """Trim a mostly uniform border while keeping enclosed background holes."""

    if image.isNull() or image.width() <= 0 or image.height() <= 0:
        raise ValueError("image must be non-null")

    width, height = image.width(), image.height()
    if max(width, height) > AUTO_CROP_ANALYSIS_MAX_PX:
        analysis = image.scaled(
            AUTO_CROP_ANALYSIS_MAX_PX,
            AUTO_CROP_ANALYSIS_MAX_PX,
            Qt.AspectRatioMode.KeepAspectRatio,
            Qt.TransformationMode.SmoothTransformation,
        )
    else:
        analysis = image

    pixels = analysis.convertToFormat(QImage.Format.Format_RGBA8888)
    analysis_width = pixels.width()
    analysis_height = pixels.height()
    data = memoryview(pixels.constBits()).cast("B")
    stride = pixels.bytesPerLine()

    border_red: list[int] = []
    border_green: list[int] = []
    border_blue: list[int] = []

    def sample(x: int, y: int) -> None:
        red, green, blue = _composited_rgb(data, y * stride + x * 4)
        border_red.append(red)
        border_green.append(green)
        border_blue.append(blue)

    border_step = max(1, (analysis_width + analysis_height) // 1024)
    for x in range(0, analysis_width, border_step):
        sample(x, 0)
        if analysis_height > 1:
            sample(x, analysis_height - 1)
    for y in range(0, analysis_height, border_step):
        sample(0, y)
        if analysis_width > 1:
            sample(analysis_width - 1, y)

    background_rgb = (
        _median(border_red),
        _median(border_green),
        _median(border_blue),
    )
    deviations = sorted(
        max(
            abs(red - background_rgb[0]),
            abs(green - background_rgb[1]),
            abs(blue - background_rgb[2]),
        )
        for red, green, blue in zip(border_red, border_green, border_blue)
    )
    border_noise = deviations[
        min(len(deviations) - 1, int(len(deviations) * 0.75))
    ]
    color_tolerance = max(
        0,
        min(72, max(int(tolerance), border_noise * 2 + 4)),
    )

    # State: 0 = subject/non-background, 1 = background candidate,
    # 2 = candidate connected to an image border.
    state = [bytearray(analysis_width) for _ in range(analysis_height)]
    for y in range(analysis_height):
        row = state[y]
        base = y * stride
        for x in range(analysis_width):
            red, green, blue = _composited_rgb(data, base + x * 4)
            if max(
                abs(red - background_rgb[0]),
                abs(green - background_rgb[1]),
                abs(blue - background_rgb[2]),
            ) <= color_tolerance:
                row[x] = 1

    background_queue: deque[int] = deque()

    def enqueue_background(x: int, y: int) -> None:
        if state[y][x] != 1:
            return
        state[y][x] = 2
        background_queue.append(y * analysis_width + x)

    for x in range(analysis_width):
        enqueue_background(x, 0)
        enqueue_background(x, analysis_height - 1)
    for y in range(analysis_height):
        enqueue_background(0, y)
        enqueue_background(analysis_width - 1, y)

    while background_queue:
        position = background_queue.popleft()
        y, x = divmod(position, analysis_width)
        if x > 0:
            enqueue_background(x - 1, y)
        if x + 1 < analysis_width:
            enqueue_background(x + 1, y)
        if y > 0:
            enqueue_background(x, y - 1)
        if y + 1 < analysis_height:
            enqueue_background(x, y + 1)

    # Ignore isolated border dirt/JPEG specks without dropping thin artwork.
    visited = [bytearray(analysis_width) for _ in range(analysis_height)]
    components: list[tuple[int, int, int, int, int]] = []
    for y in range(analysis_height):
        for x in range(analysis_width):
            if state[y][x] == 2 or visited[y][x]:
                continue
            visited[y][x] = 1
            component_queue: deque[int] = deque([y * analysis_width + x])
            area = 0
            left = right = x
            top = bottom = y
            while component_queue:
                component_position = component_queue.popleft()
                py, px = divmod(component_position, analysis_width)
                area += 1
                left = min(left, px)
                right = max(right, px)
                top = min(top, py)
                bottom = max(bottom, py)
                for ny in range(
                    max(0, py - 1),
                    min(analysis_height, py + 2),
                ):
                    for nx in range(
                        max(0, px - 1),
                        min(analysis_width, px + 2),
                    ):
                        if visited[ny][nx] or state[ny][nx] == 2:
                            continue
                        visited[ny][nx] = 1
                        component_queue.append(ny * analysis_width + nx)
            components.append((area, left, top, right, bottom))

    if not components:
        rect = QRect(0, 0, width, height)
        return AutoCropResult(image, rect, image.size())

    minimum_component_area = max(
        2,
        round(analysis_width * analysis_height * 0.00002),
    )
    largest = max(components, key=lambda component: component[0])
    kept = [
        component
        for component in components
        if component[0] >= minimum_component_area or component is largest
    ]
    min_x = min(component[1] for component in kept)
    min_y = min(component[2] for component in kept)
    max_x = max(component[3] for component in kept)
    max_y = max(component[4] for component in kept)

    scale_x = width / analysis_width
    scale_y = height / analysis_height
    padding_ratio = max(0.0, float(padding_ratio))
    desired_padding = (
        max(1, round(min(width, height) * padding_ratio))
        if padding_ratio > 0
        else 0
    )
    padding = min(max(0, int(max_padding_px)), desired_padding)
    left = max(0, math.floor(min_x * scale_x) - padding)
    top = max(0, math.floor(min_y * scale_y) - padding)
    right = min(width, math.ceil((max_x + 1) * scale_x) + padding)
    bottom = min(height, math.ceil((max_y + 1) * scale_y) + padding)
    rect = QRect(left, top, right - left, bottom - top)

    if rect == QRect(0, 0, width, height):
        return AutoCropResult(image, rect, image.size())
    return AutoCropResult(image.copy(rect), rect, image.size())
