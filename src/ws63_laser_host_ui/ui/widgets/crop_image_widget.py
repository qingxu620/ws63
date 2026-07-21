"""Interactive image-space crop selection widget."""
from __future__ import annotations

from PySide6.QtCore import QPoint, QRect, QSize, Qt, Signal
from PySide6.QtGui import QColor, QImage, QMouseEvent, QPainter, QPen
from PySide6.QtWidgets import QWidget


class CropImageWidget(QWidget):
    selection_changed = Signal(QRect)
    selection_committed = Signal(QRect)

    def __init__(self, image: QImage, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        if image.isNull():
            raise ValueError("crop image must be non-null")
        self._image = image
        self._selection = image.rect()
        self._anchor: QPoint | None = None
        self.setMinimumSize(520, 380)
        self.setMouseTracking(True)
        self.setCursor(Qt.CursorShape.CrossCursor)

    def set_image(self, image: QImage) -> None:
        if image.isNull():
            raise ValueError("crop image must be non-null")
        self._image = image
        self._selection = image.rect()
        self._anchor = None
        self.selection_changed.emit(QRect(self._selection))
        self.selection_committed.emit(QRect(self._selection))
        self.update()

    def sizeHint(self) -> QSize:
        return QSize(720, 520)

    def selection_rect(self) -> QRect:
        return QRect(self._selection)

    def image_rect(self) -> QRect:
        return self._image.rect()

    def current_image(self) -> QImage:
        return self._image

    def selected_image(self) -> QImage:
        return self._image.copy(self._selection)

    def set_selection_rect(self, rect: QRect) -> None:
        selection = rect.normalized().intersected(self._image.rect())
        if selection.isEmpty():
            selection = self._image.rect()
        self._selection = selection
        self.selection_changed.emit(QRect(selection))
        self.selection_committed.emit(QRect(selection))
        self.update()

    def reset_selection(self) -> None:
        self.set_selection_rect(self._image.rect())

    def _display_rect(self) -> QRect:
        scaled = self._image.size()
        scaled.scale(self.size(), Qt.AspectRatioMode.KeepAspectRatio)
        return QRect(
            (self.width() - scaled.width()) // 2,
            (self.height() - scaled.height()) // 2,
            scaled.width(),
            scaled.height(),
        )

    def _to_image_point(self, position: QPoint) -> QPoint:
        display = self._display_rect()
        x = round(
            (position.x() - display.left())
            * self._image.width()
            / max(1, display.width())
        )
        y = round(
            (position.y() - display.top())
            * self._image.height()
            / max(1, display.height())
        )
        return QPoint(
            max(0, min(self._image.width() - 1, x)),
            max(0, min(self._image.height() - 1, y)),
        )

    def _selection_display_rect(self, display: QRect) -> QRect:
        left = display.left() + round(
            self._selection.left() * display.width() / self._image.width()
        )
        top = display.top() + round(
            self._selection.top() * display.height() / self._image.height()
        )
        right = display.left() + round(
            (self._selection.left() + self._selection.width())
            * display.width()
            / self._image.width()
        )
        bottom = display.top() + round(
            (self._selection.top() + self._selection.height())
            * display.height()
            / self._image.height()
        )
        return QRect(left, top, max(1, right - left), max(1, bottom - top))

    def paintEvent(self, event) -> None:
        del event
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor("#111827"))
        display = self._display_rect()
        painter.drawImage(display, self._image)

        selection = self._selection_display_rect(display)
        shade = QColor(15, 23, 42, 145)
        painter.fillRect(
            QRect(
                display.left(),
                display.top(),
                display.width(),
                max(0, selection.top() - display.top()),
            ),
            shade,
        )
        painter.fillRect(
            QRect(
                display.left(),
                selection.bottom(),
                display.width(),
                max(0, display.bottom() - selection.bottom() + 1),
            ),
            shade,
        )
        painter.fillRect(
            QRect(
                display.left(),
                selection.top(),
                max(0, selection.left() - display.left()),
                selection.height(),
            ),
            shade,
        )
        painter.fillRect(
            QRect(
                selection.right(),
                selection.top(),
                max(0, display.right() - selection.right() + 1),
                selection.height(),
            ),
            shade,
        )
        painter.setPen(QPen(QColor("#38bdf8"), 2, Qt.PenStyle.SolidLine))
        painter.drawRect(selection.adjusted(1, 1, -1, -1))

        label = f"保留区域  {self._selection.width()}×{self._selection.height()} px"
        metrics = painter.fontMetrics()
        label_width = metrics.horizontalAdvance(label) + 16
        label_height = metrics.height() + 10
        label_left = min(
            max(display.left() + 4, selection.left() + 5),
            max(display.left() + 4, display.right() - label_width - 4),
        )
        label_top = min(
            max(display.top() + 4, selection.top() + 5),
            max(display.top() + 4, display.bottom() - label_height - 4),
        )
        label_rect = QRect(label_left, label_top, label_width, label_height)
        painter.fillRect(label_rect, QColor(2, 132, 199, 225))
        painter.setPen(QColor("#ffffff"))
        painter.drawText(
            label_rect.adjusted(8, 0, -8, 0),
            Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
            label,
        )

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            return
        self._anchor = self._to_image_point(event.position().toPoint())
        self._selection = QRect(self._anchor, QSize(1, 1))
        self.selection_changed.emit(QRect(self._selection))
        self.update()

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if self._anchor is None:
            return
        point = self._to_image_point(event.position().toPoint())
        left, right = sorted((self._anchor.x(), point.x()))
        top, bottom = sorted((self._anchor.y(), point.y()))
        self._selection = QRect(left, top, right - left + 1, bottom - top + 1)
        self.selection_changed.emit(QRect(self._selection))
        self.update()

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self.mouseMoveEvent(event)
            self._anchor = None
            self.selection_committed.emit(QRect(self._selection))
