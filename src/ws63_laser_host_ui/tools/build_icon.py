"""Render the application PNG into a multi-resolution Windows ICO file."""
from __future__ import annotations

import os
import struct
import sys
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import QBuffer, QIODevice, QRectF, Qt
from PySide6.QtGui import QGuiApplication, QImage, QPainter, QPainterPath


ICON_SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)


def render_png(source: QImage, size: int) -> bytes:
    image = QImage(size, size, QImage.Format.Format_ARGB32)
    image.fill(Qt.GlobalColor.transparent)

    painter = QPainter(image)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
    painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, True)

    # Keep a solid white tile behind the transparent logo, but clip only the
    # outer corners so Windows, the taskbar, and the title bar show a rounded
    # application icon instead of a hard square.
    tile = QRectF(0.0, 0.0, float(size), float(size))
    corner_radius = max(1.0, size * 0.18)
    tile_path = QPainterPath()
    tile_path.addRoundedRect(tile, corner_radius, corner_radius)
    painter.fillPath(tile_path, Qt.GlobalColor.white)

    # Keep the requested white tile while retaining a small safe area so the
    # round mark is not visually squeezed at 125–200% display scaling.
    content_size = max(1, round(size * 0.82))
    scaled = source.scaled(
        content_size,
        content_size,
        Qt.AspectRatioMode.KeepAspectRatio,
        Qt.TransformationMode.SmoothTransformation,
    )
    painter.drawImage(
        (size - scaled.width()) // 2,
        (size - scaled.height()) // 2,
        scaled,
    )
    painter.end()

    buffer = QBuffer()
    if not buffer.open(QIODevice.OpenModeFlag.WriteOnly):
        raise RuntimeError("could not allocate the PNG output buffer")
    if not image.save(buffer, "PNG"):
        raise RuntimeError(f"could not encode the {size}x{size} icon")
    return bytes(buffer.data())


def write_ico(path: Path, images: list[tuple[int, bytes]]) -> None:
    header_size = 6 + (16 * len(images))
    offset = header_size
    entries: list[bytes] = []
    payloads: list[bytes] = []

    for size, payload in images:
        dimension = 0 if size == 256 else size
        entries.append(
            struct.pack(
                "<BBBBHHII",
                dimension,
                dimension,
                0,
                0,
                1,
                32,
                len(payload),
                offset,
            )
        )
        payloads.append(payload)
        offset += len(payload)

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(
        struct.pack("<HHH", 0, 1, len(images))
        + b"".join(entries)
        + b"".join(payloads)
    )


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    source_path = root / "assets" / "app_icon.png"
    ico_path = root / "assets" / "app_icon.ico"

    app = QGuiApplication.instance() or QGuiApplication(["build-icon"])
    source = QImage(str(source_path))
    if source.isNull():
        raise RuntimeError(f"invalid PNG icon source: {source_path}")

    images = [(size, render_png(source, size)) for size in ICON_SIZES]
    write_ico(ico_path, images)
    preview_path = root / "build" / "app_icon_preview.png"
    preview_path.parent.mkdir(parents=True, exist_ok=True)
    preview_path.write_bytes(images[-1][1])
    app.processEvents()
    print(f"Generated {ico_path} ({ico_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
