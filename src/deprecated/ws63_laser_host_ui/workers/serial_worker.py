from __future__ import annotations

import threading

from PySide6.QtCore import QObject, QThread, Signal, Slot


try:
    import serial
except ImportError:  # pragma: no cover - runtime dependency check
    serial = None


class SerialWorker(QObject):
    line_received = Signal(str, str)
    status_changed = Signal(str, str)
    error_occurred = Signal(str, str)
    finished = Signal(str)

    def __init__(self, channel: str, port: str, baudrate: int = 115200, parent=None) -> None:
        super().__init__(parent)
        self.channel = channel
        self.port = port
        self.baudrate = baudrate
        self._stop_event = threading.Event()
        self._serial = None

    @Slot()
    def open(self) -> None:
        if serial is None:
            self.error_occurred.emit(self.channel, "pyserial is not installed")
            self.finished.emit(self.channel)
            return
        if not self.port:
            self.error_occurred.emit(self.channel, "no serial port selected")
            self.finished.emit(self.channel)
            return

        self._stop_event.clear()
        self.status_changed.emit(self.channel, "CONNECTING")
        try:
            self._serial = serial.Serial(
                self.port,
                self.baudrate,
                timeout=0.2,
                write_timeout=0.2,
            )
        except Exception as exc:  # pragma: no cover - hardware dependent
            self.status_changed.emit(self.channel, "ERROR")
            self.error_occurred.emit(self.channel, f"open failed on {self.port}: {exc}")
            self.finished.emit(self.channel)
            return

        self.status_changed.emit(self.channel, "CONNECTED")
        self._read_loop()

    def close(self) -> None:
        self._stop_event.set()
        if self._serial is not None:
            try:
                self._serial.close()
            except Exception:
                pass

    def _read_loop(self) -> None:
        try:
            while not self._stop_event.is_set():
                try:
                    raw = self._serial.readline() if self._serial is not None else b""
                except Exception as exc:  # pragma: no cover - hardware dependent
                    if not self._stop_event.is_set():
                        self.status_changed.emit(self.channel, "ERROR")
                        self.error_occurred.emit(self.channel, f"read failed: {exc}")
                    break

                if not raw:
                    continue

                line = raw.decode("utf-8", errors="replace").strip()
                if line:
                    self.line_received.emit(self.channel, line)
        finally:
            if self._serial is not None:
                try:
                    self._serial.close()
                except Exception:
                    pass
                self._serial = None
            self.status_changed.emit(self.channel, "DISCONNECTED")
            self.finished.emit(self.channel)


class SerialWorkerHandle:
    def __init__(self, worker: SerialWorker, thread: QThread) -> None:
        self.worker = worker
        self.thread = thread

    def stop(self) -> None:
        self.worker.close()
        self.thread.quit()
        self.thread.wait(1500)
