from __future__ import annotations

import os
import unittest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtGui import QColor, QImage
from PySide6.QtWidgets import QApplication

from app.image_crop import auto_crop_white_background


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

    def test_crop_touching_top_left_is_still_reported_as_cropped(self) -> None:
        image = QImage(20, 20, QImage.Format.Format_RGB32)
        image.fill(QColor("white"))
        for y in range(0, 8):
            for x in range(0, 10):
                image.setPixelColor(x, y, QColor("#172554"))

        result = auto_crop_white_background(image)

        self.assertTrue(result.cropped)
        self.assertEqual(result.image.size(), result.crop_rect.size())


if __name__ == "__main__":
    unittest.main()
