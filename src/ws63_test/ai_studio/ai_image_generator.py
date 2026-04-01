"""
AI 图像生成模块。

当前提供 OpenAI 兼容接口的默认实现，并保留离线 mock 兜底：
1. 如果设置了 API 地址和密钥，则走真实请求；
2. 如果未配置，则生成一张占位图，方便先联调后替换。
"""

from __future__ import annotations

import base64
import os
from pathlib import Path
from typing import Optional

import cv2
import numpy as np
import requests


class AIImageGenerationError(RuntimeError):
    """AI 生图失败时抛出的异常。"""


def _write_base64_image(image_b64: str, output_path: Path) -> Path:
    image_bytes = base64.b64decode(image_b64)
    output_path.write_bytes(image_bytes)
    return output_path


def _download_image(image_url: str, output_path: Path, timeout: float = 60.0) -> Path:
    response = requests.get(image_url, timeout=timeout)
    response.raise_for_status()
    output_path.write_bytes(response.content)
    return output_path


def _generate_mock_image(prompt: str, output_path: Path) -> Path:
    """
    离线占位图生成。

    比赛开发初期，API 还没接好时也要保证主流程能跑通，
    所以这里生成一张简单的示意图，便于联调图像处理和 G-Code。
    """

    canvas = np.full((512, 512, 3), 255, dtype=np.uint8)
    cv2.rectangle(canvas, (60, 60), (452, 452), (0, 0, 0), 3)
    cv2.circle(canvas, (256, 220), 90, (0, 0, 0), 3)
    cv2.line(canvas, (160, 360), (352, 360), (0, 0, 0), 3)
    text = prompt.strip() or "AI DEMO"
    text = text[:18]
    cv2.putText(canvas, text, (40, 490), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 0, 255), 2, cv2.LINE_AA)
    cv2.imwrite(str(output_path), canvas)
    return output_path


def generate_image_from_text(
    prompt: str,
    output_path: str = "temp_ai_image.png",
    api_base: Optional[str] = None,
    api_key: Optional[str] = None,
    model: Optional[str] = None,
    timeout: float = 90.0,
) -> str:
    """
    通过 OpenAI 兼容图片生成接口生成图像。

    环境变量约定：
    - OPENAI_BASE_URL
    - OPENAI_API_KEY
    - OPENAI_IMAGE_MODEL
    """

    prompt = prompt.strip()
    if not prompt:
        raise AIImageGenerationError("Prompt 不能为空")

    output = Path(output_path).resolve()
    api_base = (api_base or os.getenv("OPENAI_BASE_URL") or "").strip().rstrip("/")
    api_key = (api_key or os.getenv("OPENAI_API_KEY") or "").strip()
    model = (model or os.getenv("OPENAI_IMAGE_MODEL") or "gpt-image-1").strip()

    # 没有配置 API 时，使用本地 mock 图保证主流程可演示。
    if not api_base or not api_key:
        return str(_generate_mock_image(prompt, output))

    url = f"{api_base}/images/generations"
    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }
    payload = {
        "model": model,
        "prompt": prompt,
        "size": "1024x1024",
    }

    try:
        response = requests.post(url, headers=headers, json=payload, timeout=timeout)
        response.raise_for_status()
        data = response.json()
    except requests.Timeout as exc:
        raise AIImageGenerationError("AI 生图请求超时，请检查网络或稍后重试") from exc
    except requests.RequestException as exc:
        raise AIImageGenerationError(f"AI 生图请求失败: {exc}") from exc
    except ValueError as exc:
        raise AIImageGenerationError("AI 生图返回的 JSON 无法解析") from exc

    items = data.get("data") or []
    if not items:
        raise AIImageGenerationError(f"AI 生图返回为空: {data}")

    first_item = items[0]
    if "b64_json" in first_item:
        return str(_write_base64_image(first_item["b64_json"], output))
    if "url" in first_item:
        return str(_download_image(first_item["url"], output, timeout=timeout))

    raise AIImageGenerationError(f"AI 生图返回格式不受支持: {first_item}")

