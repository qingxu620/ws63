"""
Google Nano Banana 2 / Gemini 3.1 Flash Image 生图模块。

本模块负责两件事：
1. 在独立线程中调用 Gemini 3.1 Flash Image Preview REST API；
2. 解析接口返回的 base64 图片，并保存到本地供后续轮廓提取使用。
"""

from __future__ import annotations

import base64
import os
from pathlib import Path
from typing import Any, Optional

import requests

from .qt_compat import QtCore, Signal


# 这里预留给用户填写真实的 Google API Key。
API_KEY = "AIzaSyC6YrYENmDFJ4Jt1gbI6uUqBZad2EqGW4w"

MODEL_NAME = "gemini-3.1-flash-image-preview"
PROMPT_SUFFIX = (
    ", black and white line art, clean white background, "
    "vector style, highly detailed, suitable for laser engraving"
)
DEFAULT_TIMEOUT = 120.0


class AIImageGenerationError(RuntimeError):
    """AI 生图失败时抛出的异常。"""


class AIGeneratorThread(QtCore.QThread):
    """
    AI 生图线程。

    说明：
    - 网络请求必须放到子线程中，避免阻塞主界面；
    - 成功后把图片路径通过信号回传给界面；
    - 失败时把详细错误信息回传给界面。
    """

    success_signal = Signal(str)
    error_signal = Signal(str)
    started_signal = Signal(str)

    def __init__(
        self,
        prompt: str,
        output_path: str = "temp_ai_image.png",
        timeout: float = DEFAULT_TIMEOUT,
        api_key: Optional[str] = None,
        parent: Optional[QtCore.QObject] = None,
    ) -> None:
        super().__init__(parent)
        self.prompt = prompt
        self.output_path = output_path
        self.timeout = timeout
        self.api_key = api_key

    def run(self) -> None:  # pragma: no cover - GUI 线程行为
        self.started_signal.emit("正在连接 Gemini Nano Banana 2，请稍候...")
        try:
            image_path = generate_image_from_text(
                prompt=self.prompt,
                output_path=self.output_path,
                timeout=self.timeout,
                api_key=self.api_key,
            )
            self.success_signal.emit(image_path)
        except Exception as exc:
            self.error_signal.emit(str(exc))


def _resolve_api_key(override_key: Optional[str] = None) -> str:
    """优先使用调用方传入的 key，其次使用环境变量，最后使用文件顶部常量。"""

    candidates = [
        (override_key or "").strip(),
        os.getenv("GOOGLE_API_KEY", "").strip(),
        API_KEY.strip(),
    ]
    for candidate in candidates:
        if candidate and candidate != "YOUR_GOOGLE_API_KEY":
            return candidate
    raise AIImageGenerationError("请先在 ai_image_generator.py 中填入真实的 Google API Key")


def _build_request_payload(prompt: str) -> dict[str, Any]:
    """构造符合当前项目需求的 Gemini 图片生成请求体。"""

    final_prompt = prompt.strip() + PROMPT_SUFFIX
    return {
        "contents": [
            {
                "parts": [
                    {
                        "text": final_prompt,
                    }
                ]
            }
        ],
        # 官方文档说明可通过 responseModalities 强制只返回图片。
        "generationConfig": {
            "responseModalities": ["Image"],
        },
    }


def _extract_error_message(response: requests.Response) -> str:
    """尽量从 Google API 的错误响应中提取可读信息。"""

    try:
        payload = response.json()
    except ValueError:
        return response.text.strip() or f"HTTP {response.status_code}"

    error_obj = payload.get("error") or {}
    message = error_obj.get("message")
    status = error_obj.get("status")
    if message and status:
        return f"{status}: {message}"
    if message:
        return str(message)
    return str(payload)


def _extract_image_part(payload: dict[str, Any]) -> tuple[str, str]:
    """
    从 Gemini 返回 JSON 中提取第一张图片。

    官方文档示例里图片数据位于：
    candidates[0].content.parts[*].inlineData.data
    """

    prompt_feedback = payload.get("promptFeedback") or {}
    block_reason = prompt_feedback.get("blockReason")
    if block_reason:
        raise AIImageGenerationError(f"请求被模型安全策略拦截: {block_reason}")

    text_fragments: list[str] = []
    for candidate in payload.get("candidates") or []:
        content = candidate.get("content") or {}
        for part in content.get("parts") or []:
            inline_data = part.get("inlineData") or part.get("inline_data")
            if inline_data:
                image_b64 = inline_data.get("data")
                mime_type = inline_data.get("mimeType") or inline_data.get("mime_type") or "image/png"
                if image_b64:
                    return image_b64, mime_type
            if part.get("text"):
                text_fragments.append(str(part["text"]))

    if text_fragments:
        preview_text = " ".join(text_fragments).strip()
        raise AIImageGenerationError(f"接口未返回图片，只返回了文本: {preview_text[:160]}")

    raise AIImageGenerationError("接口返回中未找到图片数据，请检查模型权限、配额或请求参数")


def _guess_suffix_from_mime(mime_type: str) -> str:
    mime_lower = mime_type.lower()
    if "jpeg" in mime_lower or "jpg" in mime_lower:
        return ".jpg"
    if "webp" in mime_lower:
        return ".webp"
    return ".png"


def _save_base64_image(image_b64: str, mime_type: str, output_path: str) -> str:
    """把 base64 图片解码并保存到本地。"""

    output = Path(output_path).resolve()
    if not output.suffix:
        output = output.with_suffix(_guess_suffix_from_mime(mime_type))

    try:
        image_bytes = base64.b64decode(image_b64)
    except (ValueError, TypeError) as exc:
        raise AIImageGenerationError("接口返回的图片数据无法完成 base64 解码") from exc

    try:
        output.write_bytes(image_bytes)
    except OSError as exc:
        raise AIImageGenerationError(f"无法保存生成图片: {output}") from exc
    return str(output)


def generate_image_from_text(
    prompt: str,
    output_path: str = "temp_ai_image.png",
    timeout: float = DEFAULT_TIMEOUT,
    api_key: Optional[str] = None,
) -> str:
    """
    调用 Gemini 3.1 Flash Image Preview REST API 生成图片。

    当前实现遵循 Google 官方 REST 文档，发送 `generateContent` 请求，
    并从返回的 `inlineData.data` 中提取图片内容。
    """

    prompt = prompt.strip()
    if not prompt:
        raise AIImageGenerationError("Prompt 不能为空")

    resolved_api_key = _resolve_api_key(api_key)
    url = (
        "https://generativelanguage.googleapis.com/v1beta/models/"
        f"{MODEL_NAME}:generateContent?key={resolved_api_key}"
    )
    headers = {
        "Content-Type": "application/json",
    }
    payload = _build_request_payload(prompt)

    try:
        response = requests.post(url, headers=headers, json=payload, timeout=timeout)
    except requests.Timeout as exc:
        raise AIImageGenerationError("Gemini 生图请求超时，请检查网络或稍后重试") from exc
    except requests.RequestException as exc:
        raise AIImageGenerationError(f"Gemini 生图请求失败: {exc}") from exc

    if not response.ok:
        raise AIImageGenerationError(f"Gemini 生图接口返回错误: {_extract_error_message(response)}")

    try:
        data = response.json()
    except ValueError as exc:
        raise AIImageGenerationError("Gemini 生图返回的 JSON 无法解析") from exc

    image_b64, mime_type = _extract_image_part(data)
    return _save_base64_image(image_b64, mime_type, output_path)
