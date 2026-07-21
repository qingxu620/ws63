from __future__ import annotations

import os
import unittest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import QRect
from PySide6.QtGui import QColor, QImage
from PySide6.QtWidgets import QApplication

from app.image_crop import (
    auto_crop_white_background,
    flip_image,
    invert_image_colors,
    rotate_image_quarter_turns,
)
from ui.widgets.crop_image_widget import CropImageWidget


class ImageCropTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = QApplication.instance() or QApplication([])

    def test_crops_border_connected_white_background_with_padding(self) -> None:
        image = QImage(20, 20, QImage.Format.Format_RGB32)
        image.fill(QColor("white"))
        for y in range(8, 12):
            for x in range(7, 14):
                image.setPixelColor(x, y, QColor("#172554"))

        result = auto_crop_white_background(image)

        self.assertTrue(result.cropped)
        self.assertEqual(result.crop_rect.getRect(), (6, 7, 9, 6))
        self.assertEqual((result.image.width(), result.image.height()), (9, 6))

    def test_keeps_white_holes_inside_the_subject(self) -> None:
        image = QImage(24, 24, QImage.Format.Format_RGB32)
        image.fill(QColor("#f3f3f3"))
        for y in range(6, 18):
            for x in range(7, 17):
                image.setPixelColor(x, y, QColor("#111111"))
        for y in range(9, 15):
            for x in range(10, 14):
                image.setPixelColor(x, y, QColor("white"))

        result = auto_crop_white_background(image)

        self.assertTrue(result.cropped)
        self.assertEqual((result.image.width(), result.image.height()), (12, 14))
        self.assertEqual(result.image.pixelColor(5, 5), QColor("white"))

    def test_all_white_image_is_left_unchanged(self) -> None:
        image = QImage(12, 8, QImage.Format.Format_RGB32)
        image.fill(QColor("white"))

        result = auto_crop_white_background(image)

        self.assertFalse(result.cropped)
        self.assertEqual(result.image.size(), image.size())

    def test_crops_subject_from_uniform_colored_background(self) -> None:
        image = QImage(40, 30, QImage.Format.Format_RGB32)
        image.fill(QColor("#91a6ba"))
        for y in range(8, 22):
            for x in range(10, 30):
                image.setPixelColor(x, y, QColor("#172554"))

        result = auto_crop_white_background(image)

        self.assertTrue(result.cropped)
        self.assertEqual(result.crop_rect, QRect(9, 7, 22, 16))

    def test_transparent_background_is_composited_before_auto_crop(self) -> None:
        image = QImage(20, 16, QImage.Format.Format_ARGB32)
        image.fill(QColor(0, 0, 0, 0))
        for y in range(4, 12):
            for x in range(5, 15):
                image.setPixelColor(x, y, QColor(220, 30, 40, 255))

        result = auto_crop_white_background(image)

        self.assertTrue(result.cropped)
        self.assertEqual(result.crop_rect, QRect(4, 3, 12, 10))

    def test_auto_crop_ignores_isolated_background_speck(self) -> None:
        image = QImage(60, 40, QImage.Format.Format_RGB32)
        image.fill(QColor("#f4f3f2"))
        image.setPixelColor(1, 1, QColor("black"))
        for y in range(10, 30):
            for x in range(20, 40):
                image.setPixelColor(x, y, QColor("#1e293b"))

        result = auto_crop_white_background(image)

        self.assertTrue(result.cropped)
        self.assertEqual(result.crop_rect, QRect(19, 9, 22, 22))

    def test_crop_widget_clamps_selection_to_image_bounds(self) -> None:
        image = QImage(20, 10, QImage.Format.Format_RGB32)
        widget = CropImageWidget(image)

        widget.set_selection_rect(QRect(-5, 2, 40, 5))

        self.assertEqual(widget.selection_rect(), QRect(0, 2, 20, 5))

    def test_programmatic_crop_selection_is_committed_once(self) -> None:
        image = QImage(20, 10, QImage.Format.Format_RGB32)
        widget = CropImageWidget(image)
        changed: list[QRect] = []
        committed: list[QRect] = []
        widget.selection_changed.connect(changed.append)
        widget.selection_committed.connect(committed.append)

        widget.set_selection_rect(QRect(2, 3, 8, 4))

        self.assertEqual(changed, [QRect(2, 3, 8, 4)])
        self.assertEqual(committed, [QRect(2, 3, 8, 4)])

    def test_rotate_and_flip_preserve_asymmetric_pixel_orientation(self) -> None:
        image = QImage(3, 2, QImage.Format.Format_RGB32)
        image.fill(QColor("white"))
        image.setPixelColor(0, 0, QColor("red"))
        image.setPixelColor(2, 1, QColor("blue"))

        rotated = rotate_image_quarter_turns(image, 1)
        flipped = flip_image(image, horizontal=True)

        self.assertEqual((rotated.width(), rotated.height()), (2, 3))
        self.assertEqual(rotated.pixelColor(1, 0), QColor("red"))
        self.assertEqual(rotated.pixelColor(0, 2), QColor("blue"))
        self.assertEqual(flipped.pixelColor(2, 0), QColor("red"))
        self.assertEqual(flipped.pixelColor(0, 1), QColor("blue"))

    def test_invert_preserves_alpha(self) -> None:
        image = QImage(1, 1, QImage.Format.Format_ARGB32)
        image.setPixelColor(0, 0, QColor(10, 20, 30, 40))

        inverted = invert_image_colors(image)
        color = inverted.pixelColor(0, 0)

        self.assertEqual((color.red(), color.green(), color.blue()), (245, 235, 225))
        self.assertEqual(color.alpha(), 40)


if __name__ == "__main__":
    unittest.main()
