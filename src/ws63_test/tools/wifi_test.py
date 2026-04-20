#!/usr/bin/env python3
"""
WS63 WiFi TCP 通用验证工具。

用来替代手工 telnet/netcat 验证发射板 WiFi G-Code 入口的稳定性。
复用 uart_auto_test.py 的测试用例语义和输出格式。

用法::

    python3 wifi_test.py --host 192.168.43.1 --suite smoke
    python3 wifi_test.py --host 192.168.43.1 --suite square
    python3 wifi_test.py --host 192.168.43.1 --suite repeat --rounds 20
    python3 wifi_test.py --host 192.168.43.1 --suite all --cycles 10 --report-json result.json
"""

from __future__ import annotations

import argparse
import json
import logging
import sys
import time
from typing import Any, Dict, List, Optional, Sequence

from wifi_client import (
    WifiGcodeClient,
    ConnectionError_,
    TimeoutError_,
    parse_status_line,
)

logger = logging.getLogger("wifi_test")

# 留出更宽松的 TCP teardown 窗口，降低测试项切换时踩到旧会话尾声的概率。
TEST_RECONNECT_SETTLE_S = 0.8
CONNECT_RETRY_ATTEMPTS = 3
CONNECT_RETRY_BACKOFF_S = 0.2

# ---------------------------------------------------------------------------
# 测试结果
# ---------------------------------------------------------------------------

class TestResult:
    __slots__ = ("name", "passed", "detail", "elapsed_s")

    def __init__(self, name: str, passed: bool, detail: str, elapsed_s: float) -> None:
        self.name = name
        self.passed = passed
        self.detail = detail
        self.elapsed_s = elapsed_s

    def to_dict(self) -> Dict[str, Any]:
        return {
            "name": self.name,
            "passed": self.passed,
            "detail": self.detail,
            "elapsed_s": round(self.elapsed_s, 3),
        }


class TestFailure(Exception):
    pass

# ---------------------------------------------------------------------------
# 测试套件
# ---------------------------------------------------------------------------

def run_intro_check(client: WifiGcodeClient, timeout: float) -> TestResult:
    """验证建连后欢迎信息格式。"""
    start = time.monotonic()
    try:
        retried_after_busy = False

        while True:
            intro = client.read_intro(timeout=timeout)
            if not intro:
                raise TestFailure("no intro lines received")

            intro_busy = any(("busy another upstream host" in l) or (l == "error:busy") for l in intro)
            if intro_busy:
                if retried_after_busy:
                    raise TestFailure(f"intro busy after retry: {intro}")
                logger.warning("intro_check got busy, retry once after a short delay")
                retried_after_busy = True
                client.close()
                time.sleep(0.3)
                client.reconnect(timeout=timeout)
                continue

            has_grbl = any("Grbl" in l for l in intro)
            has_ws63 = any("WS63" in l for l in intro)
            if not has_grbl or not has_ws63:
                raise TestFailure(f"intro missing expected markers: {intro}")

            detail = f"{len(intro)} lines"
            if retried_after_busy:
                detail += " (recovered after busy retry)"
            return TestResult("intro_check", True, detail, time.monotonic() - start)
    except (TimeoutError_, ConnectionError_, TestFailure) as exc:
        return TestResult("intro_check", False, str(exc), time.monotonic() - start)


def run_wifi_status_check(client: WifiGcodeClient, timeout: float) -> TestResult:
    """验证 $WIFI? 能读到完整状态。"""
    start = time.monotonic()
    try:
        ws = client.query_wifi_status(timeout=timeout)
        required_keys = {"mode", "ip", "sle_ready", "net_ready", "tcp_listening"}
        missing = required_keys - set(ws.keys())
        if missing:
            raise TestFailure(f"missing keys: {missing}")
        return TestResult("wifi_status", True, ws.get("raw", ""), time.monotonic() - start)
    except (TimeoutError_, ConnectionError_, TestFailure) as exc:
        return TestResult("wifi_status", False, str(exc), time.monotonic() - start)


def run_smoke_suite(client: WifiGcodeClient, timeout: float) -> TestResult:
    """Smoke: 发一组基本 G-Code，全回 ok。"""
    start = time.monotonic()
    commands = ["G90", "G92", "M3 S200", "G1 X10 Y10 F6000", "G1 X20 Y10", "M5"]
    try:
        for cmd in commands:
            resp = client.send_line(cmd, timeout=timeout)
            if resp != "ok":
                raise TestFailure(f"{cmd} -> {resp}")
        # 查询状态
        status = client.query_status(timeout=timeout)
        return TestResult("smoke", True, status.get("raw", ""), time.monotonic() - start)
    except (TimeoutError_, ConnectionError_, TestFailure) as exc:
        return TestResult("smoke", False, str(exc), time.monotonic() - start)


def run_square_suite(client: WifiGcodeClient, timeout: float) -> TestResult:
    """Square: 走一圈方形轨迹。"""
    start = time.monotonic()
    commands = [
        "G90", "G92",
        "G1 X0 Y0 F6000",
        "G1 X5 Y0", "G1 X10 Y0", "G1 X15 Y0", "G1 X20 Y0",
        "G1 X20 Y5", "G1 X20 Y10",
        "G1 X15 Y10", "G1 X10 Y10", "G1 X5 Y10", "G1 X0 Y10",
        "G1 X0 Y5", "G1 X0 Y0",
    ]
    try:
        for cmd in commands:
            resp = client.send_line(cmd, timeout=timeout)
            if resp != "ok":
                raise TestFailure(f"{cmd} -> {resp}")
        return TestResult("square", True, f"{len(commands)} cmds ok", time.monotonic() - start)
    except (TimeoutError_, ConnectionError_, TestFailure) as exc:
        return TestResult("square", False, str(exc), time.monotonic() - start)


def run_repeat_suite(client: WifiGcodeClient, rounds: int, timeout: float) -> TestResult:
    """Repeat: 重复 N 轮小任务，检测粘包/丢 ok。"""
    start = time.monotonic()
    commands_per_round = ["G90", "G92", "G1 X5 Y5 F6000", "G1 X0 Y0 F6000"]
    try:
        for r in range(rounds):
            for cmd in commands_per_round:
                resp = client.send_line(cmd, timeout=timeout)
                if resp != "ok":
                    raise TestFailure(f"round {r+1}/{rounds}: {cmd} -> {resp}")
            # 每轮结束查一次状态，确认 ? 可用
            client.query_status(timeout=timeout)
        return TestResult("repeat", True, f"{rounds} rounds ok", time.monotonic() - start)
    except (TimeoutError_, ConnectionError_, TestFailure) as exc:
        return TestResult("repeat", False, str(exc), time.monotonic() - start)


def run_status_stability(client: WifiGcodeClient, count: int, timeout: float) -> TestResult:
    """连续查询 ? 多次，检查是否持续可用。"""
    start = time.monotonic()
    try:
        for i in range(count):
            status = client.query_status(timeout=timeout)
            if "state" not in status:
                raise TestFailure(f"query {i+1}: no state in response")
        return TestResult("status_stability", True, f"{count} queries ok", time.monotonic() - start)
    except (TimeoutError_, ConnectionError_, TestFailure) as exc:
        return TestResult("status_stability", False, str(exc), time.monotonic() - start)


# ---------------------------------------------------------------------------
# 套件调度
# ---------------------------------------------------------------------------

SUITE_MAP = {
    "smoke": ["intro_check", "wifi_status", "smoke"],
    "square": ["square"],
    "repeat": ["repeat"],
    "status": ["status_stability"],
    "all": ["intro_check", "wifi_status", "smoke", "square", "repeat", "status_stability"],
}


def resolve_suites(name: str) -> List[str]:
    return SUITE_MAP.get(name, [name])


def connect_with_retry(client: WifiGcodeClient, host: str, port: int, timeout: float) -> None:
    """初始建连允许做有限次短重试，吸收会话切换窗口里的瞬时失败。"""
    last_exc: Optional[ConnectionError_] = None

    for attempt in range(1, CONNECT_RETRY_ATTEMPTS + 1):
        try:
            client.connect(host, port, timeout=timeout)
            return
        except ConnectionError_ as exc:
            last_exc = exc
            if attempt >= CONNECT_RETRY_ATTEMPTS:
                break
            backoff_s = CONNECT_RETRY_BACKOFF_S * attempt
            logger.warning(
                "connect attempt %d/%d failed: %s; retry in %.1fs",
                attempt, CONNECT_RETRY_ATTEMPTS, exc, backoff_s,
            )
            time.sleep(backoff_s)

    assert last_exc is not None
    raise last_exc


def run_named_test(
    client: WifiGcodeClient,
    name: str,
    rounds: int,
    timeout: float,
) -> TestResult:
    if name == "intro_check":
        return run_intro_check(client, timeout)
    elif name == "wifi_status":
        return run_wifi_status_check(client, timeout)
    elif name == "smoke":
        return run_smoke_suite(client, timeout)
    elif name == "square":
        return run_square_suite(client, timeout)
    elif name == "repeat":
        return run_repeat_suite(client, rounds, timeout)
    elif name == "status_stability":
        return run_status_stability(client, 20, timeout)
    else:
        return TestResult(name, False, f"unknown test: {name}", 0.0)


# ---------------------------------------------------------------------------
# 报告
# ---------------------------------------------------------------------------

def write_report(
    path: str,
    args: argparse.Namespace,
    results: List[Dict[str, Any]],
    exit_code: int,
) -> None:
    report = {
        "tool": "wifi_test",
        "host": args.host,
        "port": args.port,
        "suite": args.suite,
        "rounds": args.rounds,
        "cycles": args.cycles,
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "results": results,
        "exit_code": exit_code,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, ensure_ascii=False)
    logger.info("report written to %s", path)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="WS63 WiFi TCP 通用验证工具",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
suites:
  smoke    建连 + 欢迎信息 + $WIFI? + 基本 G-Code
  square   方形轨迹
  repeat   N 轮重复任务 (检测粘包/丢 ok)
  status   连续状态查询
  all      以上全部
""",
    )
    p.add_argument("--host", default="192.168.43.1", help="板端 IP (默认 192.168.43.1)")
    p.add_argument("--port", type=int, default=5000, help="板端 TCP 端口 (默认 5000)")
    p.add_argument("--suite", default="smoke", choices=list(SUITE_MAP.keys()), help="测试套件 (默认 smoke)")
    p.add_argument("--rounds", type=int, default=10, help="repeat 套件的轮数 (默认 10)")
    p.add_argument("--cycles", type=int, default=1, help="整体循环次数 (默认 1)")
    p.add_argument("--timeout", type=float, default=5.0, help="每条命令超时 (默认 5s)")
    p.add_argument("--report-json", default=None, help="JSON 报告输出路径")
    p.add_argument("-v", "--verbose", action="store_true")
    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_arg_parser().parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [%(name)s] %(levelname)s %(message)s",
    )

    test_names = resolve_suites(args.suite)
    all_results: List[Dict[str, Any]] = []
    exit_code = 0

    for cycle in range(1, args.cycles + 1):
        cycle_label = f"[{cycle}/{args.cycles}] " if args.cycles > 1 else ""
        for test_name in test_names:
            client = WifiGcodeClient()

            try:
                connect_with_retry(client, args.host, args.port, timeout=args.timeout)
            except ConnectionError_ as exc:
                logger.error("%sconnect failed before %s: %s", cycle_label, test_name, exc)
                all_results.append(
                    {
                        "cycle": cycle,
                        "name": test_name,
                        "passed": False,
                        "detail": f"connect failed: {exc}",
                        "elapsed_s": 0,
                    }
                )
                exit_code = 1
                continue

            try:
                logger.info("%srunning: %s", cycle_label, test_name)
                result = run_named_test(client, test_name, args.rounds, args.timeout)
                d = result.to_dict()
                d["cycle"] = cycle
                all_results.append(d)

                status = "PASS" if result.passed else "FAIL"
                logger.info(
                    "%s%s %s (%.2fs) %s",
                    cycle_label, status, result.name, result.elapsed_s, result.detail,
                )
                if not result.passed:
                    exit_code = 1
            finally:
                client.close()
                # Give the board a short window to observe the TCP teardown and
                # return to the listen state before the next test reconnects.
                time.sleep(TEST_RECONNECT_SETTLE_S)

    # 汇总
    passed = sum(1 for r in all_results if r.get("passed"))
    total = len(all_results)
    logger.info("=== %d/%d PASS ===", passed, total)

    if args.report_json:
        write_report(args.report_json, args, all_results, exit_code)

    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
