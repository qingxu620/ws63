"""Doubao image generation REST client for the host UI."""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import os
from typing import Any

import requests


DOUBAO_IMAGE_ENDPOINT = "https://ark.cn-beijing.volces.com/api/v3/images/generations"
DOUBAO_IMAGE_MODEL = "doubao-seedream-5-0-260128"


class DoubaoImageError(RuntimeError):
    """User-facing Doubao image generation error."""


class MissingArkApiKeyError(DoubaoImageError):
    """Raised when ARK_API_KEY is not present in the environment."""


@dataclass(frozen=True)
class DoubaoImageResult:
    url: str
    local_path: Path


class DoubaoImageService:
    """Small REST client dedicated to Doubao image generation."""

    def __init__(
        self,
        api_key_env: str = "ARK_API_KEY",
        endpoint: str = DOUBAO_IMAGE_ENDPOINT,
        model: str = DOUBAO_IMAGE_MODEL,
        timeout_s: float = 180.0,
        session: requests.sessions.Session | None = None,
    ) -> None:
        self.api_key_env = api_key_env
        self.endpoint = endpoint
        self.model = model
        self.timeout_s = timeout_s
        self.session = session or requests.Session()

    def build_request_body(self, prompt: str, size: str, watermark: bool) -> dict[str, Any]:
        return {
            "model": self.model,
            "prompt": prompt,
            "sequential_image_generation": "disabled",
            "response_format": "url",
            "size": size,
            "stream": False,
            "watermark": watermark,
        }

    def generate_image(self, prompt: str, size: str = "2k", watermark: bool = True) -> str:
        api_key = os.environ.get(self.api_key_env, "").strip()
        if not api_key:
            raise MissingArkApiKeyError("未检测到 ARK_API_KEY，请先在系统环境变量中配置火山方舟 API Key")

        body = self.build_request_body(prompt, size, watermark)
        headers = {
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}",
        }
        try:
            response = self.session.post(
                self.endpoint,
                json=body,
                headers=headers,
                timeout=self.timeout_s,
            )
        except requests.Timeout as exc:
            raise DoubaoImageError("网络或服务响应超时，请稍后重试") from exc
        except requests.RequestException as exc:
            raise DoubaoImageError("图像生成请求失败，请检查网络连接或代理配置") from exc

        if response.status_code >= 400:
            raise DoubaoImageError(self._format_http_error(response))
        try:
            payload = response.json()
        except ValueError as exc:
            raise DoubaoImageError("图像服务返回不是有效 JSON，无法解析图片 URL") from exc
        return self.parse_image_url(payload)

    def download_image(self, url: str, output_path: Path) -> Path:
        output_path = output_path.expanduser()
        self.ensure_output_dir(output_path.parent)
        try:
            response = self.session.get(url, timeout=self.timeout_s)
        except requests.Timeout as exc:
            raise DoubaoImageError("图片下载超时，请检查网络后重试") from exc
        except requests.RequestException as exc:
            raise DoubaoImageError("图片下载失败，请检查网络连接或图片 URL 是否可访问") from exc

        if response.status_code >= 400:
            raise DoubaoImageError(self._format_http_error(response, prefix="图片下载失败"))
        content_type = response.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
        if not content_type.startswith("image/"):
            raise DoubaoImageError(
                f"图片下载失败：返回内容异常，Content-Type={content_type or '未知'}，不是 image/*"
            )
        if not response.content:
            raise DoubaoImageError("图片下载失败：服务端返回空文件")

        tmp_path = output_path.with_name(f".{output_path.name}.tmp")
        tmp_path.write_bytes(response.content)
        if tmp_path.stat().st_size <= 0:
            tmp_path.unlink(missing_ok=True)
            raise DoubaoImageError("图片下载失败：保存后的文件大小为 0")
        tmp_path.replace(output_path)
        return output_path

    @staticmethod
    def parse_image_url(payload: dict[str, Any]) -> str:
        message = DoubaoImageService.payload_error_message(payload)
        if message:
            raise DoubaoImageError(message)

        url = payload.get("url")
        if isinstance(url, str) and url.strip():
            return url.strip()

        data = payload.get("data")
        if isinstance(data, list) and data:
            first = data[0]
            if isinstance(first, dict):
                url = first.get("url")
                if isinstance(url, str) and url.strip():
                    return url.strip()
                b64_json = first.get("b64_json")
                if isinstance(b64_json, str) and b64_json.strip():
                    raise DoubaoImageError(
                        "图像服务返回了 base64 图片 data[0].b64_json；当前上位机只支持 URL 图片，"
                        "请确认 response_format=url"
                    )
        raise DoubaoImageError("图像服务返回结构不符合预期，未找到图片 URL")

    @staticmethod
    def payload_error_message(payload: dict[str, Any]) -> str:
        error = payload.get("error")
        if isinstance(error, dict):
            message = error.get("message")
            if isinstance(message, str) and message.strip():
                return message.strip()
        message = payload.get("message")
        if isinstance(message, str) and message.strip():
            return message.strip()
        return ""

    @staticmethod
    def _response_message(response: requests.Response) -> str:
        try:
            payload = response.json()
        except ValueError:
            return response.text.strip()
        if isinstance(payload, dict):
            error = payload.get("error")
            if isinstance(error, dict):
                message = error.get("message")
                if isinstance(message, str) and message.strip():
                    return message.strip()
            message = payload.get("message")
            if isinstance(message, str) and message.strip():
                return message.strip()
        return response.text.strip()

    @classmethod
    def _format_http_error(cls, response: requests.Response, prefix: str = "图像生成失败") -> str:
        detail = cls._response_message(response)
        suffix = f": {detail}" if detail else ""
        if response.status_code == 401:
            return "HTTP 401：请检查 ARK_API_KEY 是否正确，或当前账号/模型是否有权限" + suffix
        if response.status_code == 404:
            return "HTTP 404：请检查请求 URL、模型名、区域 cn-beijing 是否匹配" + suffix
        return f"{prefix}: HTTP {response.status_code}{suffix}"

    @staticmethod
    def ensure_output_dir(output_dir: Path) -> None:
        try:
            output_dir.mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            raise DoubaoImageError(f"输出目录创建失败: {output_dir}") from exc
        if not output_dir.is_dir():
            raise DoubaoImageError(f"输出路径不是目录: {output_dir}")
        if not os.access(output_dir, os.W_OK):
            raise DoubaoImageError(f"输出目录不可写: {output_dir}")
