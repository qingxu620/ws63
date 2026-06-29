"""QThread worker for Doubao image generation and download."""
from __future__ import annotations

from datetime import datetime
from pathlib import Path

from PySide6.QtCore import QThread, Signal

from app.doubao_image_service import DoubaoImageService


class DoubaoImageWorker(QThread):
    log = Signal(str, str)
    status = Signal(str)
    image_ready = Signal(str)
    error = Signal(str)

    def __init__(
        self,
        prompt: str,
        size: str,
        watermark: bool,
        output_dir: str,
        parent=None,
    ) -> None:
        super().__init__(parent)
        self.prompt = prompt
        self.size = size
        self.watermark = watermark
        self.output_dir = output_dir
        self.service = DoubaoImageService()

    def run(self) -> None:
        try:
            output_dir = Path(self.output_dir).expanduser()
            self.service.ensure_output_dir(output_dir)
            self.status.emit("正在调用豆包生图 API...")
            self.log.emit("host", f"开始豆包生图请求 size={self.size} watermark={int(self.watermark)}")
            url = self.service.generate_image(self.prompt, self.size, self.watermark)
            self.status.emit("正在下载图片...")
            stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            output_path = output_dir / f"doubao_image_{stamp}.png"
            if output_path.exists():
                stamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
                output_path = output_dir / f"doubao_image_{stamp}.png"
            saved = self.service.download_image(url, output_path)
            self.log.emit("host", f"豆包生图完成，已保存: {saved}")
            self.image_ready.emit(str(saved))
        except Exception as exc:
            self.error.emit(str(exc))
