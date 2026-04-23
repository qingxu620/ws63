"""
提示词优化模块。

职责：
1. 使用 DeepSeek 文本大模型扩写用户输入的简短短语；
2. 通过 QThread 在后台执行请求，避免阻塞主界面；
3. 通过 OpenAI 兼容 SDK 调用 DeepSeek 接口。
"""

from __future__ import annotations

from typing import Optional

try:
    from openai import APIConnectionError, APITimeoutError, AuthenticationError, OpenAI
except ImportError:  # pragma: no cover - 依赖缺失由运行时提示
    APIConnectionError = APITimeoutError = AuthenticationError = None
    OpenAI = None

from .qt_compat import QtCore, Signal


SYSTEM_PROMPT = (
    "你是一位专为图像处理与激光雕刻工作流生成提示词的 AI 专家。"
    "请将用户的简单短语，扩写为一段生图描述。"
    "【版权合规极度重要】：严禁在输出的提示词中包含任何受版权保护的具体IP名称（如迪士尼、漫威、任天堂角色、哆啦A梦等）、真实人名或品牌商标。如果用户输入了特定IP，你必须将其'翻译'为毫无侵权风险的通用外观描述（例如将'皮卡丘'改为'一只黄色的电气鼠'，将'钢铁侠'改为'穿红黄机甲的战士'）。"
    "【核心目的】：生成的图片需要有鲜艳的色彩，体现“彩色原图 -> 提取纯黑白线稿”的视觉反差效果。"
    "【画风要求】：必须是“平涂矢量插画（flat vector illustration）”、“精美卡通贴纸（die-cut sticker style）”。主体由大面积纯色块构成，带有清晰深色外轮廓边界。"
    "【绝对禁止】：严禁复杂的3D光影、柔和渐变、水彩晕染或写实细节。"
    "【背景要求】：必须是没有任何杂物的纯白背景（pure white background）。"
    "只需返回一段连贯的描述文本，不要保留用户的原始敏感词，不需要任何解释。"
)
DEFAULT_TIMEOUT = 60.0
DEFAULT_DEEPSEEK_API_KEY = "sk-9361cddb05aa40f9ab0552c365b41cb4"
DEFAULT_DEEPSEEK_BASE_URL = "https://api.deepseek.com"
DEFAULT_DEEPSEEK_TEXT_MODEL = "deepseek-chat"
DEFAULT_MAX_TOKENS = 200


class PromptOptimizerError(RuntimeError):
    """提示词优化失败时抛出的异常。"""


class PromptOptimizerThread(QtCore.QThread):
    """后台执行 DeepSeek 文本扩写请求。"""

    started_signal = Signal(str)
    finished_signal = Signal(str)
    error_signal = Signal(str)

    def __init__(
        self,
        prompt: str,
        timeout: float = DEFAULT_TIMEOUT,
        api_key: Optional[str] = None,
        model_name: Optional[str] = None,
        base_url: Optional[str] = None,
        parent: Optional[QtCore.QObject] = None,
    ) -> None:
        super().__init__(parent)
        self.prompt = prompt
        self.timeout = timeout
        self.api_key = api_key
        self.model_name = model_name
        self.base_url = base_url

    def run(self) -> None:  # pragma: no cover - GUI 线程行为
        self.started_signal.emit("正在调用 DeepSeek 文本模型优化提示词，请稍候...")
        try:
            optimized_prompt = optimize_prompt_text(
                prompt=self.prompt,
                timeout=self.timeout,
                api_key=self.api_key,
                model_name=self.model_name,
                base_url=self.base_url,
            )
            self.finished_signal.emit(optimized_prompt)
        except Exception as exc:
            self.error_signal.emit(str(exc))


def _resolve_deepseek_api_key(override_key: Optional[str] = None) -> str:
    candidate = (override_key or "").strip()
    if candidate:
        return candidate
    if DEFAULT_DEEPSEEK_API_KEY.strip():
        return DEFAULT_DEEPSEEK_API_KEY.strip()
    raise PromptOptimizerError("DeepSeek API Key 未配置，无法使用一键美化提示词")


def _resolve_deepseek_base_url(override_base_url: Optional[str] = None) -> str:
    candidate = (override_base_url or "").strip()
    if candidate:
        return candidate.rstrip("/")
    return DEFAULT_DEEPSEEK_BASE_URL


def _resolve_deepseek_text_model(override_model_name: Optional[str] = None) -> str:
    candidate = (override_model_name or "").strip()
    if candidate:
        return candidate
    return DEFAULT_DEEPSEEK_TEXT_MODEL


def _extract_text_from_completion(response: object) -> str:
    choices = getattr(response, "choices", None) or []
    if not choices:
        raise PromptOptimizerError("提示词优化接口返回为空")

    first_choice = choices[0]
    message = getattr(first_choice, "message", None)
    content = getattr(message, "content", None)
    if isinstance(content, str) and content.strip():
        return content.strip()
    raise PromptOptimizerError("提示词优化接口未返回有效文本")


def optimize_prompt_text(
    prompt: str,
    *,
    timeout: float = DEFAULT_TIMEOUT,
    api_key: Optional[str] = None,
    model_name: Optional[str] = None,
    base_url: Optional[str] = None,
) -> str:
    """使用 OpenAI 兼容 SDK 调用 DeepSeek chat/completions 接口扩写提示词。"""

    user_prompt = prompt.strip()
    if not user_prompt:
        raise PromptOptimizerError("请先输入需要美化的提示词")
    if OpenAI is None:
        raise PromptOptimizerError("缺少 openai 依赖，请先安装 openai 包")

    resolved_api_key = _resolve_deepseek_api_key(api_key)
    resolved_model_name = _resolve_deepseek_text_model(model_name)
    resolved_base_url = _resolve_deepseek_base_url(base_url)
    client = OpenAI(
        api_key=resolved_api_key,
        base_url=resolved_base_url,
        timeout=timeout,
    )

    try:
        response = client.chat.completions.create(
            model=resolved_model_name,
            messages=[
                {"role": "system", "content": SYSTEM_PROMPT},
                {"role": "user", "content": user_prompt},
            ],
            temperature=0.7,
            max_tokens=DEFAULT_MAX_TOKENS,
        )
    except Exception as exc:
        if APITimeoutError is not None and isinstance(exc, APITimeoutError):
            raise PromptOptimizerError("提示词优化请求超时，请检查网络或稍后重试") from exc
        if APIConnectionError is not None and isinstance(exc, APIConnectionError):
            raise PromptOptimizerError(f"提示词优化请求失败: {exc}") from exc
        if AuthenticationError is not None and isinstance(exc, AuthenticationError):
            raise PromptOptimizerError("DeepSeek API Key 无效或已失效") from exc
        raise PromptOptimizerError(f"提示词优化请求失败: {exc}") from exc

    optimized_prompt = _extract_text_from_completion(response)
    if not optimized_prompt:
        raise PromptOptimizerError("提示词优化结果为空，请稍后重试")
    return optimized_prompt
