"""
任务管理模块 —— AIGC 云边协同打标的核心调度引擎。

本模块实现计划文档中要求的全部任务管理能力：
1. 任务状态机：IDLE -> AI_GENERATING -> CONTOUR_EXTRACTING -> GCODE_GENERATING -> SENDING -> COMPLETED / FAILED
2. 产物管理：每次任务自动保存 AI 原图、轮廓图、G-Code 和任务日志
3. 历史记录：支持"最近任务"快速复用，所有成功/失败任务均可追溯
4. 可读反馈：为界面层提供面向演示的中文状态描述
"""

from __future__ import annotations

import json
import shutil
import time
from dataclasses import dataclass, field, asdict
from enum import Enum
from pathlib import Path
from typing import Callable, Dict, List, Optional


class TaskState(Enum):
    """任务状态机，覆盖从 Prompt 到执行完成的完整生命周期。"""

    IDLE = "idle"
    AI_GENERATING = "ai_generating"
    CONTOUR_EXTRACTING = "contour_extracting"
    GCODE_GENERATING = "gcode_generating"
    SENDING = "sending"
    COMPLETED = "completed"
    FAILED = "failed"


# 面向演示的中文状态描述，用于界面显示和日志
STATE_DISPLAY_TEXT: Dict[TaskState, str] = {
    TaskState.IDLE: "⏳ 等待任务",
    TaskState.AI_GENERATING: "🎨 AI 图像生成中…",
    TaskState.CONTOUR_EXTRACTING: "✏️ 轮廓提取中…",
    TaskState.GCODE_GENERATING: "⚙️ G-Code 生成中…",
    TaskState.SENDING: "📡 下发到设备中…",
    TaskState.COMPLETED: "✅ 任务执行完成",
    TaskState.FAILED: "❌ 任务失败",
}

# 状态进度百分比，用于进度条
STATE_PROGRESS: Dict[TaskState, int] = {
    TaskState.IDLE: 0,
    TaskState.AI_GENERATING: 20,
    TaskState.CONTOUR_EXTRACTING: 50,
    TaskState.GCODE_GENERATING: 80,
    TaskState.SENDING: 90,
    TaskState.COMPLETED: 100,
    TaskState.FAILED: 0,
}

# 面向评委和演示的阶段详情描述
STATE_DETAIL_TEXT: Dict[TaskState, str] = {
    TaskState.IDLE: "系统就绪，等待用户发起新任务。",
    TaskState.AI_GENERATING: "正在调用云端生图模型生成彩色原图，预计 10~30 秒…",
    TaskState.CONTOUR_EXTRACTING: "正在使用 OpenCV 进行 CLAHE + Canny 边缘提取与轮廓归一化…",
    TaskState.GCODE_GENERATING: "正在将轮廓路径映射到物理坐标并生成 G-Code 指令序列…",
    TaskState.SENDING: "正在逐行下发 G-Code 到 WS63 边缘设备，等待每行 ok 确认…",
    TaskState.COMPLETED: "全部指令已成功执行，激光打标任务完成。",
    TaskState.FAILED: "任务执行过程中出现错误，请查看详细日志。",
}


@dataclass
class TaskRecord:
    """单次任务的完整记录，用于历史回溯和快速复用。"""

    task_id: str = ""
    template_name: str = ""
    prompt: str = ""
    image_model: str = ""
    state: str = TaskState.IDLE.value
    create_time: str = ""
    finish_time: str = ""
    duration_seconds: float = 0.0
    status: str = "running"
    error_msg: str = ""
    completed_at: str = ""

    # 产物路径（相对于任务目录的文件名）
    ai_image_file: str = ""
    contour_image_file: str = ""
    gcode_file: str = ""

    # 雕刻参数快照
    width_mm: float = 50.0
    height_mm: float = 50.0
    feed_rate: int = 6000
    power: int = 200

    # 连接方式
    connection_mode: str = ""
    connection_target: str = ""

    # 结果
    gcode_line_count: int = 0
    contour_count: int = 0
    success: bool = False
    error_message: str = ""

    # 源图路径（用于本地导入场景）
    source_image_path: str = ""
    source_type: str = ""  # "ai" | "local"

    def to_dict(self) -> dict:
        return asdict(self)

    @classmethod
    def from_dict(cls, data: dict) -> "TaskRecord":
        known_fields = {f.name for f in cls.__dataclass_fields__.values()}
        filtered = {k: v for k, v in data.items() if k in known_fields}
        return cls(**filtered)


def _generate_task_id() -> str:
    """基于时间戳生成唯一任务 ID，格式易读且适合做文件夹名。"""
    return time.strftime("%Y%m%d_%H%M%S")


def _format_time() -> str:
    return time.strftime("%Y-%m-%d %H:%M:%S")


def _format_duration(seconds: float) -> str:
    """格式化持续时间为可读字符串。"""
    if seconds < 60:
        return f"{seconds:.1f} 秒"
    minutes = int(seconds // 60)
    secs = seconds % 60
    return f"{minutes} 分 {secs:.1f} 秒"


class TaskManager:
    """
    任务管理器 —— 管理任务生命周期、产物和历史。

    存储结构：
    ~/.ai_studio/
      ├── tasks/
      │   ├── 20260419_193500/
      │   │   ├── task.json          # 任务元数据
      │   │   ├── ai_original.png    # AI 原图
      │   │   ├── contour.png        # 轮廓图
      │   │   └── output.gcode       # G-Code
      │   ├── 20260419_194200/
      │   │   └── ...
      │   └── ...
      └── history.json               # 所有任务索引
    """

    MAX_HISTORY_DISPLAY = 20

    def __init__(self, base_dir: Optional[str] = None) -> None:
        if base_dir:
            self._base_dir = Path(base_dir)
        else:
            self._base_dir = Path.home() / ".ai_studio"

        self._tasks_dir = self._base_dir / "tasks"
        self._tasks_dir.mkdir(parents=True, exist_ok=True)

        self._current_state = TaskState.IDLE
        self._current_record: Optional[TaskRecord] = None
        self._current_task_dir: Optional[Path] = None
        self._start_time: float = 0.0
        self._history: List[TaskRecord] = []
        self._state_listeners: List[Callable[["TaskManager"], None]] = []

        self._load_history()

    # ── 状态机 ──────────────────────────────────────────────

    @property
    def state(self) -> TaskState:
        return self._current_state

    @property
    def state_display(self) -> str:
        return STATE_DISPLAY_TEXT.get(self._current_state, "")

    @property
    def state_detail(self) -> str:
        return STATE_DETAIL_TEXT.get(self._current_state, "")

    @property
    def state_progress(self) -> int:
        return STATE_PROGRESS.get(self._current_state, 0)

    @property
    def current_record(self) -> Optional[TaskRecord]:
        return self._current_record

    @property
    def current_task_dir(self) -> Optional[Path]:
        return self._current_task_dir

    def is_busy(self) -> bool:
        """任务是否正在进行中。"""
        return self._current_state not in (TaskState.IDLE, TaskState.COMPLETED, TaskState.FAILED)

    def add_state_listener(self, listener: Callable[["TaskManager"], None]) -> None:
        """注册状态变更回调。"""
        if listener not in self._state_listeners:
            self._state_listeners.append(listener)

    def remove_state_listener(self, listener: Callable[["TaskManager"], None]) -> None:
        """移除状态变更回调。"""
        if listener in self._state_listeners:
            self._state_listeners.remove(listener)

    def begin_task(
        self,
        template_name: str = "",
        prompt: str = "",
        image_model: str = "",
        source_type: str = "ai",
        width_mm: float = 50.0,
        height_mm: float = 50.0,
        feed_rate: int = 6000,
        power: int = 200,
        connection_mode: str = "",
        connection_target: str = "",
    ) -> TaskRecord:
        """
        开始一个新任务，创建任务目录并初始化记录。

        调用此方法后，状态机将进入 AI_GENERATING（AI 模式）或 CONTOUR_EXTRACTING（本地导入模式）。
        """
        task_id = _generate_task_id()
        task_dir = self._tasks_dir / task_id
        task_dir.mkdir(parents=True, exist_ok=True)

        record = TaskRecord(
            task_id=task_id,
            template_name=template_name,
            prompt=prompt,
            image_model=image_model,
            create_time=_format_time(),
            source_type=source_type,
            width_mm=width_mm,
            height_mm=height_mm,
            feed_rate=feed_rate,
            power=power,
            connection_mode=connection_mode,
            connection_target=connection_target,
        )

        self._current_record = record
        self._current_task_dir = task_dir
        self._start_time = time.monotonic()

        if source_type == "ai":
            self._transition(TaskState.AI_GENERATING)
        else:
            self._transition(TaskState.CONTOUR_EXTRACTING)

        self._save_current_task()
        return record

    def on_ai_image_ready(self, image_path: str) -> str:
        """AI 图像生成完成，复制产物到任务目录。"""
        if self._current_record is None or self._current_task_dir is None:
            return image_path

        dest = self._current_task_dir / f"ai_original{Path(image_path).suffix}"
        try:
            shutil.copy2(image_path, dest)
            self._current_record.ai_image_file = dest.name
        except OSError:
            self._current_record.ai_image_file = str(image_path)

        self._transition(TaskState.CONTOUR_EXTRACTING)
        self._save_current_task()
        return str(dest) if dest.exists() else image_path

    def on_local_image_loaded(self, image_path: str) -> str:
        """本地图片导入完成，复制到任务目录。"""
        if self._current_record is None or self._current_task_dir is None:
            return image_path

        dest = self._current_task_dir / f"source_image{Path(image_path).suffix}"
        try:
            shutil.copy2(image_path, dest)
            self._current_record.ai_image_file = dest.name
            self._current_record.source_image_path = str(image_path)
        except OSError:
            self._current_record.ai_image_file = str(image_path)

        self._save_current_task()
        return str(dest) if dest.exists() else image_path

    def on_contour_ready(self, contour_path: str, contour_count: int) -> str:
        """轮廓提取完成，保存轮廓图。"""
        if self._current_record is None or self._current_task_dir is None:
            return contour_path

        dest = self._current_task_dir / f"contour{Path(contour_path).suffix}"
        try:
            shutil.copy2(contour_path, dest)
            self._current_record.contour_image_file = dest.name
        except OSError:
            self._current_record.contour_image_file = str(contour_path)

        self._current_record.contour_count = contour_count
        self._transition(TaskState.GCODE_GENERATING)
        self._save_current_task()
        return str(dest) if dest.exists() else contour_path

    def on_gcode_ready(self, gcode_lines: List[str]) -> str:
        """G-Code 生成完成，保存到任务目录。"""
        if self._current_record is None or self._current_task_dir is None:
            return ""

        gcode_path = self._current_task_dir / "output.gcode"
        try:
            gcode_path.write_text("\n".join(gcode_lines) + "\n", encoding="utf-8")
            self._current_record.gcode_file = gcode_path.name
        except OSError:
            pass

        self._current_record.gcode_line_count = len(gcode_lines)
        self._transition(TaskState.SENDING)
        self._save_current_task()
        return str(gcode_path)

    def on_sending_started(self) -> None:
        """设备下发已开始。"""
        if self._current_state != TaskState.SENDING:
            self._transition(TaskState.SENDING)
        self._save_current_task()

    def on_task_completed(self) -> None:
        """任务成功完成。"""
        if self._current_record is None:
            return
        self._current_record.success = True
        self._current_record.status = "success"
        self._current_record.error_msg = ""
        self._current_record.error_message = ""
        self._current_record.finish_time = _format_time()
        self._current_record.completed_at = self._current_record.finish_time
        self._current_record.duration_seconds = time.monotonic() - self._start_time
        self._transition(TaskState.COMPLETED)
        self._finalize_task()

    def on_task_failed(self, error_message: str) -> None:
        """任务失败。"""
        if self._current_record is None:
            return
        self._current_record.success = False
        self._current_record.status = "failed"
        self._current_record.error_msg = error_message
        self._current_record.error_message = error_message
        self._current_record.finish_time = _format_time()
        self._current_record.completed_at = self._current_record.finish_time
        self._current_record.duration_seconds = time.monotonic() - self._start_time
        self._transition(TaskState.FAILED)
        self._finalize_task()

    def reset_to_idle(self) -> None:
        """重置状态机为空闲。"""
        self._current_state = TaskState.IDLE
        self._current_record = None
        self._current_task_dir = None
        self._notify_state_changed()

    # ── 历史管理 ──────────────────────────────────────────────

    @property
    def history(self) -> List[TaskRecord]:
        return list(self._history)

    def recent_successful_tasks(self, count: int = 5) -> List[TaskRecord]:
        """获取最近成功的任务，用于快速复用。"""
        return [r for r in self._history if r.success][:count]

    def get_task_artifacts_dir(self, task_id: str) -> Optional[Path]:
        """获取指定任务的产物目录。"""
        task_dir = self._tasks_dir / task_id
        if task_dir.is_dir():
            return task_dir
        return None

    def get_task_record(self, task_id: str) -> Optional[TaskRecord]:
        """获取指定任务的记录。"""
        for record in self._history:
            if record.task_id == task_id:
                return record
        return None

    def format_task_summary(self, record: TaskRecord) -> str:
        """生成面向用户的任务摘要。"""
        status = "✅ 成功" if record.success else "❌ 失败"
        source = "AI" if record.source_type == "ai" else "本地"
        duration = _format_duration(record.duration_seconds) if record.duration_seconds > 0 else "—"
        template = record.template_name or "自定义"

        lines = [
            f"任务编号: {record.task_id}",
            f"状态: {status}",
            f"来源: {source}",
            f"模板: {template}",
            f"创建时间: {record.create_time}",
            f"耗时: {duration}",
            f"轮廓数: {record.contour_count}",
            f"G-Code 行数: {record.gcode_line_count}",
            f"尺寸: {record.width_mm:.1f} × {record.height_mm:.1f} mm",
            f"连接: {record.connection_mode} → {record.connection_target}",
        ]

        if not record.success and record.error_message:
            lines.append(f"失败原因: {record.error_message}")

        return "\n".join(lines)

    def get_readable_error(self, error_message: str) -> str:
        """将技术错误消息转换为面向用户的可读提示。"""
        error_lower = error_message.lower()

        if "timeout" in error_lower or "超时" in error_message:
            return "⏰ 操作超时：云端 AI 接口响应较慢，请检查网络连接后重试。如果频繁超时，可切换到本地图片导入模式。"
        if "api key" in error_lower or "key" in error_lower:
            return "🔑 API 密钥问题：请确认当前生图服务的 API Key 已正确填入并且有效。"
        if "连接" in error_message or "connect" in error_lower:
            return "🔌 设备连接失败：请检查串口线缆是否松动，或 WiFi 网络是否正常。"
        if "急停" in error_message or "emergency" in error_lower:
            return "🛑 任务已被急停终止：操作员手动触发了紧急停止，激光已关闭，队列已清空。"
        if "轮廓" in error_message or "contour" in error_lower:
            return "✏️ 轮廓提取失败：当前图片不适合提取轮廓。建议使用线条清晰、背景简洁的图案，或切换到其他比赛模板。"
        if "串口" in error_message or "serial" in error_lower:
            return "🔌 串口通信异常：请检查串口连接并确认波特率设置正确。"
        if "wifi" in error_lower or "network" in error_lower or "网络" in error_message:
            return "📡 WiFi 通信异常：请检查 WiFi 连接状态和 IP 地址配置。"
        if "ok" in error_lower and "超时" in error_message:
            return "⏰ 设备响应超时：WS63 未在规定时间内返回 ok 确认。请检查设备运行状态。"
        if "error" in error_lower and "下位机" in error_message:
            return f"⚠️ WS63 返回错误：{error_message}。请检查命令格式或设备状态。"

        return f"⚠️ {error_message}"

    # ── 内部方法 ──────────────────────────────────────────────

    def _transition(self, new_state: TaskState) -> None:
        """执行状态转换。"""
        self._current_state = new_state
        if self._current_record is not None:
            self._current_record.state = new_state.value
        self._notify_state_changed()

    def _notify_state_changed(self) -> None:
        """向界面层广播状态变更。"""
        for listener in list(self._state_listeners):
            try:
                listener(self)
            except Exception:
                pass

    def _save_current_task(self) -> None:
        """保存当前任务的元数据到磁盘。"""
        if self._current_record is None or self._current_task_dir is None:
            return
        task_file = self._current_task_dir / "task.json"
        try:
            task_file.write_text(
                json.dumps(self._current_record.to_dict(), ensure_ascii=False, indent=2),
                encoding="utf-8",
            )
        except OSError:
            pass

    def _save_final_meta(self) -> None:
        """在任务结束时把归档摘要写入 meta.json。"""
        if self._current_record is None or self._current_task_dir is None:
            return
        meta_file = self._current_task_dir / "meta.json"
        meta_payload = self._current_record.to_dict()
        meta_payload.update(
            {
                "status": self._current_record.status or ("success" if self._current_record.success else "failed"),
                "error_msg": self._current_record.error_msg or self._current_record.error_message,
                "completed_at": self._current_record.completed_at or self._current_record.finish_time,
            }
        )
        try:
            meta_file.write_text(
                json.dumps(meta_payload, ensure_ascii=False, indent=2),
                encoding="utf-8",
            )
        except OSError:
            pass

    def _finalize_task(self) -> None:
        """任务结束后保存最终状态并更新历史。"""
        self._save_current_task()
        self._save_final_meta()
        if self._current_record is not None:
            # 插入到历史最前面
            self._history.insert(0, self._current_record)
            self._save_history()

    def _load_history(self) -> None:
        """从磁盘加载历史记录索引。"""
        history_file = self._base_dir / "history.json"
        if not history_file.exists():
            self._history = []
            return

        try:
            data = json.loads(history_file.read_text(encoding="utf-8"))
            self._history = [TaskRecord.from_dict(item) for item in data]
        except (OSError, json.JSONDecodeError, TypeError):
            self._history = []

    def _save_history(self) -> None:
        """保存历史记录索引到磁盘。"""
        history_file = self._base_dir / "history.json"
        # 只保留最近的记录
        records_to_save = self._history[: self.MAX_HISTORY_DISPLAY * 2]
        try:
            history_file.write_text(
                json.dumps(
                    [r.to_dict() for r in records_to_save],
                    ensure_ascii=False,
                    indent=2,
                ),
                encoding="utf-8",
            )
        except OSError:
            pass
