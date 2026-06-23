"""
G-code normalize and validate pipeline for WS63 Laser Host UI.

All G-code sources must pass through this pipeline before upload:
- LaserGRBL file import
- Host UI vectorization (raster/contour)
- Preview then upload
- Save then upload
"""
from __future__ import annotations

import math
import re
from dataclasses import dataclass

# ---- Constants ----

GCODE_LINE_MAX_BYTES = 120  # RX SLE_JOB_GCODE_LINE_MAX=128, leave 8-byte safety margin
RX_WORK_AREA_MM = 70.0
LASER_S_MAX = 1000.0
TX_UART_RESYNC_CHAR = "\x18"

SUPPORTED_GCODE_COMMANDS = {
    "G0", "G00", "G1", "G01", "G21", "G90", "G91", "G28", "G92",
    "M3", "M5", "M30",
}

UNSUPPORTED_GCODE_COMMANDS = {
    "G2", "G3", "G4", "G17", "G18", "G19", "G54", "G55", "M4",
}

# Pattern to extract G/M commands from a line
_CMD_RE = re.compile(r"^(?:G|M)\d+(?:\.\d+)?", re.IGNORECASE)


@dataclass
class GcodeDiagnostic:
    """Diagnostic information about a G-code payload."""
    source: str
    total_bytes: int
    line_count: int
    max_line_len: int
    crc16: int
    has_eof_newline: bool
    first_lines: list[str]
    last_lines: list[str]
    errors: list[str]
    warnings: list[str]
    use_preroll: bool
    preroll_bytes: int


def _crc16_ccitt(data: bytes, initial: int = 0xFFFF) -> int:
    crc = initial & 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc & 0xFFFF


def normalize_gcode(text: str) -> str:
    """Normalize G-code text for upload.

    Rules:
    - Unify line endings to \\n
    - Ensure trailing newline
    - Remove empty lines
    - Remove comments after ;
    - Reject parenthesis comments ()
    - Convert to uppercase
    """
    # Unify line endings
    text = text.replace("\r\n", "\n").replace("\r", "\n")

    lines = text.split("\n")
    output: list[str] = []

    for line in lines:
        # Remove comments after ;
        line = line.split(";", 1)[0]

        # Check for parenthesis comments
        if "(" in line:
            raise RuntimeError(f"不支持括号注释 '(...)'，请使用 ';' 注释或移除括号注释")

        # Strip whitespace
        line = line.strip()

        # Skip empty lines
        if not line:
            continue

        # Convert to uppercase
        line = line.upper()

        # RX uses millimetres unconditionally; keep the established upload
        # contract by removing standalone metric-mode declarations.
        if re.fullmatch(r"G21(?:\.0+)?", line):
            continue

        output.append(line)

    # Ensure trailing newline
    return "\n".join(output) + "\n"


def validate_gcode(text: str) -> list[str]:
    """Validate G-code text. Returns list of errors (empty if valid).

    Checks:
    - File not empty
    - Line length <= 120 bytes
    - No NaN/inf
    - No %
    - No unsupported commands (G2, G3, G4, G17, G18, G19, G54, G55, M4)
    - X/Y/F/S values parseable
    - F > 0
    - S in 0-1000
    - X/Y in 0-70mm
    """
    errors: list[str] = []
    lines = text.strip().split("\n")

    if not lines or not any(line.strip() for line in lines):
        errors.append("G-code 文件为空")
        return errors

    for i, line in enumerate(lines, start=1):
        line = line.strip()
        if not line:
            continue

        if TX_UART_RESYNC_CHAR in line:
            errors.append(f"第 {i} 行包含保留的 UART 同步控制字节 0x18")
            continue

        # Check line length
        line_bytes = len(line.encode("utf-8"))
        if line_bytes > GCODE_LINE_MAX_BYTES:
            errors.append(f"第 {i} 行超过长度限制: {line_bytes}/{GCODE_LINE_MAX_BYTES} bytes")
            continue

        # Check for % (program start/end marker)
        if "%" in line:
            errors.append(f"第 {i} 行包含 '%' 字符，不支持")
            continue

        # Check for NaN/inf
        if "NAN" in line or "INF" in line:
            errors.append(f"第 {i} 行包含 NaN 或 inf")
            continue

        # Check for unsupported commands
        words = line.split()
        for word in words:
            cmd_match = _CMD_RE.match(word)
            if cmd_match:
                cmd = cmd_match.group(0).upper()
                # Normalize G00 -> G0, G01 -> G1
                if cmd.startswith("G") and cmd[1:].startswith("0") and len(cmd) > 2 and cmd[2:].isdigit():
                    cmd = "G" + cmd[2:].lstrip("0") or "G0"
                if cmd in UNSUPPORTED_GCODE_COMMANDS:
                    errors.append(f"第 {i} 行包含不支持的命令: {cmd}")
                elif cmd.startswith("G") and cmd not in SUPPORTED_GCODE_COMMANDS and not cmd.startswith("G"):
                    # Only warn for unknown G commands, not M commands
                    pass

        # Check X/Y coordinates
        for coord in ["X", "Y"]:
            if coord in line:
                try:
                    # Find the value after the coordinate letter
                    idx = line.index(coord)
                    rest = line[idx + 1:]
                    # Extract the number
                    num_str = ""
                    for c in rest:
                        if c.isdigit() or c in ".+-":
                            num_str += c
                        else:
                            break
                    if num_str:
                        val = float(num_str)
                        if math.isnan(val) or math.isinf(val):
                            errors.append(f"第 {i} 行 {coord} 值无效: {num_str}")
                        elif val < 0 or val > RX_WORK_AREA_MM:
                            errors.append(f"第 {i} 行 {coord} 超出工作范围: {val:.3f} (0-{RX_WORK_AREA_MM}mm)")
                except (ValueError, IndexError):
                    errors.append(f"第 {i} 行 {coord} 值无法解析")

        # Check F (feed rate)
        if "F" in line:
            try:
                idx = line.index("F")
                rest = line[idx + 1:]
                num_str = ""
                for c in rest:
                    if c.isdigit() or c in ".+-":
                        num_str += c
                    else:
                        break
                if num_str:
                    val = float(num_str)
                    if val <= 0:
                        errors.append(f"第 {i} 行 F 值必须大于 0: {val}")
            except (ValueError, IndexError):
                errors.append(f"第 {i} 行 F 值无法解析")

        # Check S (laser power)
        if "S" in line:
            try:
                idx = line.index("S")
                rest = line[idx + 1:]
                num_str = ""
                for c in rest:
                    if c.isdigit() or c in ".+-":
                        num_str += c
                    else:
                        break
                if num_str:
                    val = float(num_str)
                    if val < 0 or val > LASER_S_MAX:
                        errors.append(f"第 {i} 行 S 值超出范围: {val} (0-{LASER_S_MAX})")
            except (ValueError, IndexError):
                errors.append(f"第 {i} 行 S 值无法解析")

    return errors


def prepare_gcode_for_upload(text: str, source: str = "unknown", preroll_bytes: int = 4096) -> tuple[bytes, GcodeDiagnostic]:
    """Normalize and validate G-code, return payload bytes and diagnostic info.

    This is the single entry point for all G-code upload preparation.
    """
    # Normalize
    normalized = normalize_gcode(text)

    # Validate
    errors = validate_gcode(normalized)
    if errors:
        raise RuntimeError(f"G-code 校验失败:\n" + "\n".join(f"  - {e}" for e in errors))

    # Convert to bytes
    payload = normalized.encode("ascii")

    # Check for non-ASCII characters (should not happen after normalize, but double-check)
    try:
        normalized.encode("ascii")
    except UnicodeEncodeError as e:
        raise RuntimeError(f"G-code 包含非 ASCII 字符: {e}")

    # Build diagnostic
    lines = normalized.strip().split("\n")
    line_lengths = [len(line.encode("utf-8")) for line in lines if line.strip()]
    max_line_len = max(line_lengths) if line_lengths else 0

    first_lines = [line for line in lines[:5] if line.strip()]
    last_lines = [line for line in lines[-5:] if line.strip()]

    use_preroll = len(payload) > preroll_bytes

    diag = GcodeDiagnostic(
        source=source,
        total_bytes=len(payload),
        line_count=len([l for l in lines if l.strip()]),
        max_line_len=max_line_len,
        crc16=_crc16_ccitt(payload),
        has_eof_newline=normalized.endswith("\n"),
        first_lines=first_lines,
        last_lines=last_lines,
        errors=errors,
        warnings=[],
        use_preroll=use_preroll,
        preroll_bytes=preroll_bytes if use_preroll else 0,
    )

    return payload, diag


def format_diagnostic(diag: GcodeDiagnostic) -> str:
    """Format diagnostic info for logging."""
    lines = [
        f"=== G-code 诊断 ===",
        f"来源: {diag.source}",
        f"字节: {diag.total_bytes}",
        f"行数: {diag.line_count}",
        f"最大行长: {diag.max_line_len}/{GCODE_LINE_MAX_BYTES}",
        f"CRC16: 0x{diag.crc16:04X}",
        f"EOF换行: {diag.has_eof_newline}",
        f"路径: {'preroll' if diag.use_preroll else 'normal upload'}",
    ]
    if diag.use_preroll:
        lines.append(f"preroll: {diag.preroll_bytes} bytes")
    lines.append(f"--- 前 5 行 ---")
    for line in diag.first_lines:
        lines.append(f"  {line}")
    lines.append(f"--- 后 5 行 ---")
    for line in diag.last_lines:
        lines.append(f"  {line}")
    return "\n".join(lines)
