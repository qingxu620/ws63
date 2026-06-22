"""
WorkerManager — thread-safe wrappers for SleJobSerialClient operations.

Each public method spawns a QThread that runs the operation and emits signals
on completion. The UI never blocks.
"""
from __future__ import annotations

from typing import Optional

from PySide6.QtCore import QObject, QThread, Signal


class WorkerSignals(QObject):
    log = Signal(str, str)
    finished = Signal()
    error = Signal(str)
    progress = Signal(str)
    task_status = Signal(str, float)


class _WorkerThread(QThread):
    """Generic worker thread that runs a callable and emits signals."""

    def __init__(self, fn, *args, **kwargs):
        super().__init__()
        self._fn = fn
        self._args = args
        self._kwargs = kwargs
        self.signals = WorkerSignals()

    def run(self):
        try:
            self._fn(*self._args, **self._kwargs)
        except Exception as exc:
            self.signals.error.emit(str(exc))
        finally:
            self.signals.finished.emit()


class WorkerManager(QObject):
    """Manages background worker threads for serial operations."""

    log = Signal(str, str)
    progress = Signal(str)
    task_status = Signal(str, float)
    finished = Signal()

    def __init__(self, parent: Optional[QObject] = None) -> None:
        super().__init__(parent)
        self._thread: Optional[_WorkerThread] = None

    def is_busy(self) -> bool:
        return self._thread is not None and self._thread.isRunning()

    def run(self, fn, *args, **kwargs) -> None:
        if self.is_busy():
            self.log.emit("error", "任务进行中，请等待完成")
            return
        t = _WorkerThread(fn, *args, **kwargs)
        t.signals.log.connect(self.log)
        t.signals.progress.connect(self.progress)
        t.signals.task_status.connect(self.task_status)
        t.signals.finished.connect(self._on_finished)
        t.signals.error.connect(lambda msg: self.log.emit("error", msg))
        self._thread = t
        t.start()

    def wait_for_idle(self, timeout_ms: int) -> bool:
        thread = self._thread
        if thread is None or not thread.isRunning():
            return True
        return thread.wait(max(0, timeout_ms))

    def _on_finished(self) -> None:
        self._thread = None
        self.finished.emit()
