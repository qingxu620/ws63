"""Cancellable asynchronous Doubao image generation and download."""
from __future__ import annotations

from datetime import datetime
import json
import os
from pathlib import Path
from typing import Any

from PySide6.QtCore import QByteArray, QObject, QUrl, Signal
from PySide6.QtNetwork import QNetworkAccessManager, QNetworkReply, QNetworkRequest

from app.doubao_image_service import DoubaoImageError, DoubaoImageService


class DoubaoImageWorker(QObject):
    """Run the two HTTP requests without blocking and allow immediate abort."""

    log = Signal(str, str)
    status = Signal(str)
    image_ready = Signal(str)
    error = Signal(str)
    cancelled = Signal()
    finished = Signal()

    def __init__(
        self,
        prompt: str,
        size: str,
        watermark: bool,
        output_dir: str,
        parent: QObject | None = None,
        *,
        service: DoubaoImageService | None = None,
        network_manager: QNetworkAccessManager | None = None,
    ) -> None:
        super().__init__(parent)
        self.prompt = prompt
        self.size = size
        self.watermark = watermark
        self.output_dir = output_dir
        self.service = service or DoubaoImageService()
        self._network = network_manager or QNetworkAccessManager(self)
        self._reply: QNetworkReply | None = None
        self._running = False
        self._cancel_requested = False
        self._stage = "idle"

    def isRunning(self) -> bool:
        """Compatibility with the previous QThread-backed worker."""

        return self._running

    def start(self) -> None:
        if self._running:
            return
        api_key = os.environ.get(self.service.api_key_env, "").strip()
        if not api_key:
            self.error.emit(
                "未检测到 ARK_API_KEY，请先在系统环境变量中配置火山方舟 API Key"
            )
            self.finished.emit()
            return
        try:
            output_dir = Path(self.output_dir).expanduser()
            self.service.ensure_output_dir(output_dir)
            body = self.service.build_request_body(
                self.prompt,
                self.size,
                self.watermark,
            )
        except Exception as exc:
            self.error.emit(str(exc))
            self.finished.emit()
            return

        request = self._request(self.service.endpoint)
        request.setHeader(
            QNetworkRequest.KnownHeaders.ContentTypeHeader,
            "application/json",
        )
        request.setRawHeader(
            QByteArray(b"Authorization"),
            QByteArray(f"Bearer {api_key}".encode("utf-8")),
        )
        payload = QByteArray(
            json.dumps(body, ensure_ascii=False).encode("utf-8")
        )
        self._running = True
        self._cancel_requested = False
        self._stage = "generate"
        self.status.emit("正在调用豆包生图 API...")
        self.log.emit(
            "host",
            f"开始豆包生图请求 size={self.size} watermark={int(self.watermark)}",
        )
        self._reply = self._network.post(request, payload)
        self._reply.finished.connect(self._on_generation_finished)

    def cancel(self) -> None:
        if not self._running:
            return
        self._cancel_requested = True
        stage = "API 请求" if self._stage == "generate" else "图片下载"
        self.log.emit("status", f"正在取消豆包生图：中止{stage}")
        self._finish(cancelled=True)

    def _request(self, url: str) -> QNetworkRequest:
        request = QNetworkRequest(QUrl(url))
        request.setTransferTimeout(max(1, int(self.service.timeout_s * 1000)))
        return request

    def _on_generation_finished(self) -> None:
        reply = self._reply
        if reply is None or not self._running:
            return
        if self._cancel_requested:
            self._finish(cancelled=True)
            return
        try:
            raw = self._read_successful_reply(reply, "图像生成失败")
            try:
                payload = json.loads(raw.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                raise DoubaoImageError(
                    "图像服务返回不是有效 JSON，无法解析图片 URL"
                ) from exc
            if not isinstance(payload, dict):
                raise DoubaoImageError("图像服务返回结构不符合预期")
            url = self.service.parse_image_url(payload)
        except Exception as exc:
            self._fail(str(exc))
            return
        finally:
            reply.deleteLater()

        self._stage = "download"
        self.status.emit("正在下载图片...")
        self._reply = self._network.get(self._request(url))
        self._reply.finished.connect(self._on_download_finished)

    def _on_download_finished(self) -> None:
        reply = self._reply
        if reply is None or not self._running:
            return
        if self._cancel_requested:
            self._finish(cancelled=True)
            return
        try:
            content = self._read_successful_reply(reply, "图片下载失败")
            content_type = str(
                reply.header(QNetworkRequest.KnownHeaders.ContentTypeHeader) or ""
            ).split(";", 1)[0].strip().lower()
            if not content_type.startswith("image/"):
                raise DoubaoImageError(
                    "图片下载失败：返回内容异常，"
                    f"Content-Type={content_type or '未知'}，不是 image/*"
                )
            if not content:
                raise DoubaoImageError("图片下载失败：服务端返回空文件")
            output_path = self._next_output_path()
            tmp_path = output_path.with_name(f".{output_path.name}.tmp")
            try:
                tmp_path.write_bytes(content)
                if tmp_path.stat().st_size <= 0:
                    raise DoubaoImageError("图片下载失败：保存后的文件大小为 0")
                tmp_path.replace(output_path)
            except Exception:
                tmp_path.unlink(missing_ok=True)
                raise
            self.log.emit("host", f"豆包生图完成，已保存: {output_path}")
            self.image_ready.emit(str(output_path))
        except Exception as exc:
            self._fail(str(exc))
            return
        finally:
            reply.deleteLater()
        self._finish()

    def _next_output_path(self) -> Path:
        output_dir = Path(self.output_dir).expanduser()
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        output_path = output_dir / f"doubao_image_{stamp}.png"
        if output_path.exists():
            stamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
            output_path = output_dir / f"doubao_image_{stamp}.png"
        return output_path

    def _read_successful_reply(
        self,
        reply: QNetworkReply,
        prefix: str,
    ) -> bytes:
        status_value = reply.attribute(
            QNetworkRequest.Attribute.HttpStatusCodeAttribute
        )
        status = int(status_value) if status_value is not None else 0
        raw = bytes(reply.readAll())
        if reply.error() != QNetworkReply.NetworkError.NoError:
            if reply.error() == QNetworkReply.NetworkError.OperationCanceledError:
                raise DoubaoImageError("请求已取消")
            raise DoubaoImageError(
                f"{prefix}：{reply.errorString() or '网络连接异常'}"
            )
        if status >= 400:
            detail = self._payload_message(raw)
            suffix = f": {detail}" if detail else ""
            if status == 401:
                raise DoubaoImageError(
                    "HTTP 401：请检查 ARK_API_KEY 是否正确，或当前账号/模型是否有权限"
                    + suffix
                )
            if status == 404:
                raise DoubaoImageError(
                    "HTTP 404：请检查请求 URL、模型名、区域 cn-beijing 是否匹配"
                    + suffix
                )
            raise DoubaoImageError(f"{prefix}: HTTP {status}{suffix}")
        return raw

    @staticmethod
    def _payload_message(raw: bytes) -> str:
        try:
            payload: Any = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return raw.decode("utf-8", errors="replace").strip()
        if isinstance(payload, dict):
            return DoubaoImageService.payload_error_message(payload)
        return ""

    def _fail(self, message: str) -> None:
        if self._cancel_requested:
            self._finish(cancelled=True)
            return
        self.error.emit(message)
        self._finish()

    def _finish(self, *, cancelled: bool = False) -> None:
        if not self._running:
            return
        self._running = False
        self._stage = "idle"
        reply = self._reply
        self._reply = None
        if reply is not None and reply.isRunning():
            reply.abort()
        if cancelled:
            self.cancelled.emit()
        self.finished.emit()
