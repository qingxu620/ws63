#!/usr/bin/env python3
"""
WS63 WiFi 轻量桥接脚本 —— 串口语义转 TCP。

把原本面向串口的逐行 G-Code 任务语义搬到 WiFi TCP 入口。
不模拟串口设备，不对接 LaserGRBL。

用法::

    # 文件模式：发送 .gcode 文件
    python3 wifi_bridge.py --host 192.168.43.1 --file demo.gcode

    # 管道模式
    cat demo.gcode | python3 wifi_bridge.py --host 192.168.43.1

    # 交互模式 (默认，会自动生成会话日志)
    python3 wifi_bridge.py --host 192.168.43.1 --interactive

    # 指定日志文件
    python3 wifi_bridge.py --host 192.168.43.1 --interactive --log-file session.log
"""

from __future__ import annotations

import argparse
import logging
import os
import sys
import time
from typing import Optional, Sequence, TextIO

from wifi_client import (
    WifiGcodeClient,
    ConnectionError_,
    TimeoutError_,
    parse_status_line,
    parse_wifi_status_line,
)

logger = logging.getLogger("wifi_bridge")

# ---------------------------------------------------------------------------
# 日志文件
# ---------------------------------------------------------------------------

def setup_log_file(path: str) -> logging.FileHandler:
    handler = logging.FileHandler(path, encoding="utf-8")
    handler.setLevel(logging.DEBUG)
    handler.setFormatter(logging.Formatter("%(asctime)s %(message)s"))
    logging.getLogger().addHandler(handler)
    return handler

# ---------------------------------------------------------------------------
# 文件/管道模式
# ---------------------------------------------------------------------------

def run_file_mode(client: WifiGcodeClient, source: TextIO, timeout: float) -> int:
    """逐行读取 source 并发送，打印每行的响应。"""
    lines = source.read().splitlines()
    total = len(lines)
    sent = 0
    errors = 0

    for i, raw in enumerate(lines):
        stripped = raw.strip()
        # 跳过空行和注释
        if not stripped or stripped.startswith(";") or stripped.startswith("("):
            continue
        # 去行内注释
        if ";" in stripped:
            stripped = stripped[: stripped.index(";")].rstrip()
        if not stripped:
            continue

        try:
            resp = client.send_line(stripped, timeout=timeout)
            sent += 1
            status = "ok" if resp == "ok" else resp
            print(f"[{sent}/{total}] {stripped} -> {status}")
            if resp.startswith("error"):
                errors += 1
                logger.warning("error at line %d: %s -> %s", i + 1, stripped, resp)
        except TimeoutError_ as exc:
            errors += 1
            print(f"[{sent}/{total}] {stripped} -> TIMEOUT: {exc}")
            logger.error("timeout at line %d: %s", i + 1, stripped)
        except ConnectionError_ as exc:
            print(f"\n连接断开: {exc}")
            return 1

    print(f"\n完成: {sent} 行已发送, {errors} 个错误")
    return 1 if errors > 0 else 0

# ---------------------------------------------------------------------------
# 交互模式
# ---------------------------------------------------------------------------

def run_interactive_mode(client: WifiGcodeClient, timeout: float) -> int:
    """交互式命令输入，支持历史记录。"""
    # 尝试启用 readline 历史
    try:
        import readline  # noqa: F401
    except ImportError:
        pass

    print("WS63 WiFi Bridge 交互模式")
    print("输入 G-Code 命令，按 Enter 发送")
    print("特殊命令: /status  /wifi  /quit  /help")
    print()

    while True:
        try:
            line = input("ws63> ").strip()
        except (EOFError, KeyboardInterrupt):
            print("\n退出")
            return 0

        if not line:
            continue

        # 内置命令
        if line.startswith("/"):
            cmd = line.lower()
            if cmd in ("/quit", "/exit", "/q"):
                return 0
            elif cmd == "/help":
                print("  /status   查询运动状态 (?)")
                print("  /wifi     查询 WiFi 状态 ($WIFI?)")
                print("  /quit     退出")
                continue
            elif cmd == "/status":
                line = "?"
            elif cmd == "/wifi":
                line = "$WIFI?"
            else:
                print(f"未知命令: {line}")
                continue

        # ? 走专用路径
        if line == "?":
            try:
                status = client.query_status(timeout=timeout)
                print(f"  {status.get('raw', '')}")
                print(f"  状态={status['state']}  X={status['x']:.3f}  Y={status['y']:.3f}")
            except (TimeoutError_, ConnectionError_) as exc:
                print(f"  错误: {exc}")
            continue

        # $WIFI? 走专用路径
        if line in ("$WIFI?", "$WIFI"):
            try:
                ws = client.query_wifi_status(timeout=timeout)
                print(f"  {ws.get('raw', '')}")
                sle_str = "✓" if ws.get("sle_ready") else "✗"
                print(f"  模式={ws['mode']}  IP={ws['ip']}  SLE={sle_str}")
            except (TimeoutError_, ConnectionError_) as exc:
                print(f"  错误: {exc}")
            continue

        # 普通命令
        try:
            resp = client.send_line(line, timeout=timeout)
            print(f"  {resp}")
        except TimeoutError_ as exc:
            print(f"  超时: {exc}")
        except ConnectionError_ as exc:
            print(f"  连接断开: {exc}")
            return 1

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="WS63 WiFi 轻量桥接脚本 — 串口语义转 TCP",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
示例:
  python3 wifi_bridge.py --host 192.168.43.1 --file demo.gcode
  cat commands.txt | python3 wifi_bridge.py --host 192.168.43.1
  python3 wifi_bridge.py --host 192.168.43.1 --interactive

说明:
  交互模式默认生成会话日志；文件模式和管道模式默认不生成。
""",
    )
    p.add_argument("--host", default="192.168.43.1", help="板端 IP (默认 192.168.43.1)")
    p.add_argument("--port", type=int, default=5000, help="板端 TCP 端口 (默认 5000)")
    p.add_argument("--file", "-f", default=None, help=".gcode 文件路径")
    p.add_argument("--interactive", "-i", action="store_true", help="启动交互模式 (默认生成会话日志)")
    p.add_argument("--timeout", type=float, default=5.0, help="每条命令超时 (默认 5s)")
    p.add_argument("--log-file", default=None, help="会话日志文件路径")
    p.add_argument("-v", "--verbose", action="store_true")
    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_arg_parser().parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [%(name)s] %(levelname)s %(message)s",
    )

    # 确定模式
    use_file = args.file is not None
    use_interactive = args.interactive or (not use_file and sys.stdin.isatty())

    if args.log_file:
        setup_log_file(args.log_file)
    elif use_interactive:
        ts = time.strftime("%Y%m%d_%H%M%S")
        log_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), f"wifi_bridge_{ts}.log")
        setup_log_file(log_path)
        logger.info("session log: %s", log_path)

    # 连接
    client = WifiGcodeClient()
    try:
        print(f"连接 {args.host}:{args.port} ...")
        client.connect(args.host, args.port, timeout=args.timeout)
    except ConnectionError_ as exc:
        print(f"连接失败: {exc}", file=sys.stderr)
        return 1

    try:
        # 读欢迎信息
        intro = client.read_intro(timeout=args.timeout)
        for line in intro:
            print(f"  {line}")

        if any(("busy another upstream host" in line) or (line == "error:busy") for line in intro):
            print("\n当前板端已有其他上游客户端连接，请先断开旧连接后再重试。")
            return 1

        # 自动查一次 $WIFI?
        try:
            ws = client.query_wifi_status(timeout=3)
            sle_str = "✓" if ws.get("sle_ready") else "✗"
            print(f"  WiFi: {ws.get('mode','?')} IP={ws.get('ip','?')} SLE={sle_str}")
        except (TimeoutError_, ConnectionError_):
            print("  (无法获取 WiFi 状态)")

        print()

        if use_file:
            with open(args.file, "r", encoding="utf-8") as f:
                return run_file_mode(client, f, args.timeout)
        elif use_interactive:
            return run_interactive_mode(client, args.timeout)
        else:
            # stdin 管道
            return run_file_mode(client, sys.stdin, args.timeout)
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
