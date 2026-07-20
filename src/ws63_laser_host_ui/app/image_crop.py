"""Image cleanup helpers for generated artwork."""
from __future__ import annotations

from collections import deque
from dataclasses import dataclass

from PySide6.QtCore import QRect, QSize
from PySide6.QtGui import QColor, QImage


@dataclass(frozen=True)
class AutoCropResult:
    """Result of removing border-connected near-white background."""

    image: QImage
    crop_rect: QRect
    source_size: QSize

    @property
    def cropped(self) -> bool:
        return self.crop_rect != QRect(
            0, 0, self.source_size.width(), self.source_size.height()
        )


def _is_border_background(color: QColor, tolerance: int) -> bool:
    """Return whether a pixel can belong to a white/transparent outer border."""
    if color.alpha() <= 12:
        return True
    red, green, blue = color.red(), color.green(), color.blue()
    # AI services commonly return white backgrounds with a small warm/cool tint.
    # Restrict the relaxed rule to near-neutral light colors so pale subject
    # colors are not discarded globally.
    if min(red, green, blue) >= 245:
        return True
    return (
        min(red, green, blue) >= 220
        and max(red, green, blue) - min(red, green, blue) <= tolerance
    )


def auto_crop_white_background(
    image: QImage,
    *,
    tolerance: int = 18,
    padding_ratio: float = 0.02,
    max_padding_px: int = 16,
) -> AutoCropResult:
    """Crop only border-connected near-white pixels from an AI-generated image.

    White areas inside the subject are kept because background detection starts
    at the image border and follows only connected background pixels.
    """
    if image.isNull() or image.width() <= 0 or image.height() <= 0:
        raise ValueError("image must be non-null")

    width, height = image.width(), image.height()
    pixels = image.convertToFormat(QImage.Format.Format_ARGB32)
    tolerance = max(0, min(64, int(tolerance)))
    background = [[False] * width for _ in range(height)]
    queue: deque[tuple[int, int]] = deque()

    def enqueue(x: int, y: int) -> None:
        if background[y][x]:
            return
        if not _is_border_background(pixels.pixelColor(x, y), tolerance):
            return
        background[y][x] = True
        queue.append((x, y))

    for x in range(width):
        enqueue(x, 0)
        enqueue(x, height - 1)
    for y in range(height):
        enqueue(0, y)
        enqueue(width - 1, y)

    while queue:
        x, y = queue.popleft()
        if x > 0:
            enqueue(x - 1, y)
        if x + 1 < width:
            enqueue(x + 1, y)
        if y > 0:
            enqueue(x, y - 1)
        if y + 1 < height:
            enqueue(x, y + 1)

    min_x, min_y = width, height
    max_x = max_y = -1
    for y in range(height):
        for x in range(width):
            if background[y][x]:
                continue
            min_x = min(min_x, x)
            max_x = max(max_x, x)
            min_y = min(min_y, y)
            max_y = max(max_y, y)
    if max_x < 0:
        rect = QRect(0, 0, width, height)
        return AutoCropResult(image, rect, image.size())

    padding = min(
        max(0, int(max_padding_px)),
        max(1, round(min(width, height) * max(0.0, padding_ratio))),
    )
    left = max(0, min_x - padding)
    top = max(0, min_y - padding)
    right = min(width - 1, max_x + padding)
    bottom = min(height - 1, max_y + padding)
    rect = QRect(left, top, right - left + 1, bottom - top + 1)
    if rect == QRect(0, 0, width, height):
        return AutoCropResult(image, rect, image.size())
    return AutoCropResult(image.copy(rect), rect, image.size())
