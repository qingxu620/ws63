"""
彩色生图模块。

本模块负责两件事：
1. 在独立线程中调用通义万相 2.7 或豆包的 REST API 完成生图；
2. 下载生成图片并保存到本地，供后续轮廓提取与任务归档使用。
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Optional
from urllib.parse import urlparse

import requests

from .qt_compat import QtCore, Signal


WANX_API_KEY = "sk-f6abf096797741e38eea01c6c97b2330"
ARK_API_KEY = "ark-16930b57-4b98-421b-9cd5-d73a10602da1-401c1"

WANX_GENERATION_URL = "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation"
DOUBAO_GENERATION_URL = "https://ark.cn-beijing.volces.com/api/v3/images/generations"

MODEL_OPTIONS: dict[str, dict[str, str]] = {
    "通义万相 2.7": {
        "model_id": "wan2.7-image",
        "backend": "wanx",
    },
    "豆包 Seedream 5.0": {
        "model_id": "doubao-seedream-5-0-260128",
        "backend": "doubao",
    },
}
DEFAULT_MODEL_LABEL = "通义万相 2.7"
PROMPT_SUFFIX = (
    "，请采用色彩鲜艳的平涂矢量插画风格，主体由大面积纯色块构成，带有极度清晰的深色外轮廓边界线，"
    "纯白干净背景，严禁出现3D光影渐变、立体质感和复杂写实细节，极其适合作为剪贴画或提取完美边缘使用。"
)
DEFAULT_TIMEOUT = 180.0


class AIImageGenerationError(RuntimeError):
    """AI 生图失败时抛出的异常。"""


@dataclass
class GeneratedImageResult:
    """返回给主界面的生成结果。"""

    image_path: str
    image_bytes: bytes
    mime_type: str


class AIGeneratorThread(QtCore.QThread):
    """
    AI 生图线程。

    说明：
    - 网络请求必须放到子线程中，避免阻塞主界面；
    - 成功后把图片路径和图片字节通过信号回传给界面；
    - 失败时把详细错误信息回传给界面。
    """

    success_signal = Signal(object)
    error_signal = Signal(str)
    started_signal = Signal(str)

    def __init__(
        self,
        prompt: str,
        model_label: str = DEFAULT_MODEL_LABEL,
        output_path: str = "temp_ai_image.png",
        timeout: float = DEFAULT_TIMEOUT,
        api_key: Optional[str] = None,
        parent: Optional[QtCore.QObject] = None,
    ) -> None:
        super().__init__(parent)
        self.prompt = prompt
        self.model_label = model_label
        self.output_path = output_path
        self.timeout = timeout
        self.api_key = api_key

    def run(self) -> None:  # pragma: no cover - GUI 线程行为
        self.started_signal.emit(f"正在连接 {self.model_label}，请稍候...")
        try:
            result = generate_image_from_text(
                prompt=self.prompt,
                model_label=self.model_label,
                output_path=self.output_path,
                timeout=self.timeout,
                api_key=self.api_key,
            )
            self.success_signal.emit(result)
        except Exception as exc:
            self.error_signal.emit(str(exc))


def _resolve_wanx_api_key(override_key: Optional[str] = None) -> str:
    candidate = (override_key or "").strip()
    if candidate:
        return candidate
    if WANX_API_KEY.strip():
        return WANX_API_KEY.strip()
    raise AIImageGenerationError("请先在 ai_image_generator.py 中填入通义万相 API Key")


def _resolve_doubao_api_key(override_key: Optional[str] = None) -> str:
    candidates = [
        (override_key or "").strip(),
        os.getenv("ARK_API_KEY", "").strip(),
        ARK_API_KEY.strip(),
    ]
    for candidate in candidates:
        if candidate:
            return candidate
    raise AIImageGenerationError("请先在 ai_image_generator.py 中填入豆包 Ark API Key")


def _build_final_prompt(prompt: str) -> str:
    return prompt.strip() + PROMPT_SUFFIX


def _build_wanx_request_payload(prompt: str, model_name: str) -> dict[str, Any]:
    final_prompt = _build_final_prompt(prompt)
    return {
        "model": model_name,
        "input": {
            "messages": [
                {
                    "role": "user",
                    "content": [
                        {"text": final_prompt},
                    ],
                }
            ]
        },
        "parameters": {
            "size": "1024*1024",
            "watermark": False,
        },
    }


def _build_doubao_request_payload(prompt: str, model_name: str) -> dict[str, Any]:
    final_prompt = _build_final_prompt(prompt)
    return {
        "model": model_name,
        "prompt": final_prompt,
        "sequential_image_generation": "disabled",
        "response_format": "url",
        "size": "2K",
        "stream": False,
        "watermark": True,
    }


def _extract_error_message(response: requests.Response) -> str:
    try:
        payload = response.json()
    except ValueError:
        return response.text.strip() or f"HTTP {response.status_code}"

    error_obj = payload.get("error")
    if isinstance(error_obj, dict):
        message = error_obj.get("message")
        if message:
            return str(message)

    code = str(payload.get("code") or "").strip()
    message = str(payload.get("message") or "").strip()
    if code and message:
        return f"{code}: {message}"
    if message:
        return message
    return str(payload)


def _guess_suffix_from_mime(mime_type: str) -> str:
    mime_lower = mime_type.lower()
    if "jpeg" in mime_lower or "jpg" in mime_lower:
        return ".jpg"
    if "webp" in mime_lower:
        return ".webp"
    return ".png"


def _save_image_bytes(image_bytes: bytes, mime_type: str, output_path: str, image_url: str = "") -> str:
    output = Path(output_path).resolve()
    if not output.suffix:
        suffix = _guess_suffix_from_mime(mime_type)
        remote_suffix = Path(urlparse(image_url).path).suffix if image_url else ""
        output = output.with_suffix(remote_suffix or suffix)

    try:
        output.write_bytes(image_bytes)
    except OSError as exc:
        raise AIImageGenerationError(f"无法保存生成图片: {output}") from exc
    return str(output)


def _download_image_result(
    image_url: str,
    output_path: str,
    *,
    timeout: float,
    source_name: str,
) -> GeneratedImageResult:
    try:
        response = requests.get(image_url, timeout=timeout)
        response.raise_for_status()
    except requests.Timeout as exc:
        raise AIImageGenerationError(f"{source_name} 图片下载超时，请稍后重试") from exc
    except requests.RequestException as exc:
        raise AIImageGenerationError(f"无法下载 {source_name} 生成图片: {exc}") from exc

    image_bytes = response.content
    if not image_bytes:
        raise AIImageGenerationError(f"{source_name} 返回的图片内容为空")

    mime_type = response.headers.get("Content-Type", "image/png")
    image_path = _save_image_bytes(image_bytes, mime_type, output_path, image_url=image_url)
    return GeneratedImageResult(image_path=image_path, image_bytes=image_bytes, mime_type=mime_type)


def _extract_wanx_image_url(payload: dict[str, Any]) -> str:
    output = payload.get("output") or {}
    results = output.get("results") or []
    if results and isinstance(results[0], dict):
        image_url = str(results[0].get("url") or "").strip()
        if image_url:
            return image_url

    # 兼容阿里云当前官方文档中的同步返回结构：output.choices[].message.content[].image
    choices = output.get("choices") or []
    if choices and isinstance(choices[0], dict):
        message = choices[0].get("message") or {}
        content = message.get("content") or []
        for item in content:
            if not isinstance(item, dict):
                continue
            image_url = str(item.get("image") or "").strip()
            if image_url:
                return image_url

    raise AIImageGenerationError(f"通义万相 2.7 返回中未找到图片 URL: {payload}")


def resolve_model_config(model_label: Optional[str]) -> dict[str, str]:
    selected = (model_label or "").strip() or DEFAULT_MODEL_LABEL
    return MODEL_OPTIONS.get(selected, MODEL_OPTIONS[DEFAULT_MODEL_LABEL]).copy()


def _generate_wanx_via_rest(
    prompt: str,
    *,
    output_path: str,
    timeout: float,
    api_key: str,
    model_name: str,
) -> GeneratedImageResult:
    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }
    payload = _build_wanx_request_payload(prompt, model_name)

    try:
        response = requests.post(WANX_GENERATION_URL, headers=headers, json=payload, timeout=timeout)
    except requests.Timeout as exc:
        raise AIImageGenerationError("通义万相 2.7 生图请求超时，请检查网络或稍后重试") from exc
    except requests.RequestException as exc:
        raise AIImageGenerationError(f"通义万相 2.7 生图请求失败: {exc}") from exc

    if not response.ok:
        raise AIImageGenerationError(f"通义万相 2.7 接口返回错误: {_extract_error_message(response)}")

    try:
        data = response.json()
    except ValueError as exc:
        raise AIImageGenerationError("通义万相 2.7 返回的 JSON 无法解析") from exc

    image_url = _extract_wanx_image_url(data)
    return _download_image_result(image_url, output_path, timeout=timeout, source_name="通义万相 2.7")


def _generate_doubao_via_rest(
    prompt: str,
    *,
    output_path: str,
    timeout: float,
    api_key: str,
    model_name: str,
) -> GeneratedImageResult:
    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {api_key}",
    }
    payload = _build_doubao_request_payload(prompt, model_name)

    try:
        response = requests.post(DOUBAO_GENERATION_URL, headers=headers, json=payload, timeout=timeout)
    except requests.Timeout as exc:
        raise AIImageGenerationError("豆包 Seedream 5.0 生图请求超时，请检查网络或稍后重试") from exc
    except requests.RequestException as exc:
        raise AIImageGenerationError(f"豆包 Seedream 5.0 生图请求失败: {exc}") from exc

    if not response.ok:
        raise AIImageGenerationError(f"豆包 Seedream 5.0 接口返回错误: {_extract_error_message(response)}")

    try:
        data = response.json()
    except ValueError as exc:
        raise AIImageGenerationError("豆包 Seedream 5.0 返回的 JSON 无法解析") from exc

    image_items = data.get("data") or []
    if not image_items or not isinstance(image_items[0], dict):
        raise AIImageGenerationError(f"豆包 Seedream 5.0 返回中未找到图片 URL: {data}")

    image_url = str(image_items[0].get("url") or "").strip()
    if not image_url:
        raise AIImageGenerationError(f"豆包 Seedream 5.0 返回中未找到有效图片 URL: {data}")

    return _download_image_result(image_url, output_path, timeout=timeout, source_name="豆包 Seedream 5.0")


def generate_image_from_text(
    prompt: str,
    model_label: str = DEFAULT_MODEL_LABEL,
    output_path: str = "temp_ai_image.png",
    timeout: float = DEFAULT_TIMEOUT,
    api_key: Optional[str] = None,
) -> GeneratedImageResult:
    """
    根据前端选择的模型动态调用通义万相 2.7 或豆包图片接口。
    """

    prompt = prompt.strip()
    if not prompt:
        raise AIImageGenerationError("Prompt 不能为空")

    model_config = resolve_model_config(model_label)
    backend = model_config["backend"]
    if backend == "doubao":
        model_name = os.getenv("ARK_IMAGE_MODEL", "").strip() or model_config["model_id"]
        resolved_api_key = _resolve_doubao_api_key(api_key)
        return _generate_doubao_via_rest(
            prompt,
            output_path=output_path,
            timeout=timeout,
            api_key=resolved_api_key,
            model_name=model_name,
        )

    model_name = model_config["model_id"]
    resolved_api_key = _resolve_wanx_api_key(api_key)
    return _generate_wanx_via_rest(
        prompt,
        output_path=output_path,
        timeout=timeout,
        api_key=resolved_api_key,
        model_name=model_name,
    )
