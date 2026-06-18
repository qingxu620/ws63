#!/usr/bin/env python3
"""
WS63 long-running stress test wrapper.

This tool reuses uart_auto_test.py and runs one or more suites for many cycles,
which is useful after smoke bring-up when you want a soak test result that can
be attached to a project delivery record.

Typical usage:

  python3 tools/stress_test.py /dev/ttyUSB1 --suite repeat --rounds 20 --cycles 50
  python3 tools/stress_test.py /dev/ttyUSB1 --tx-debug-port /dev/ttyUSB0 --suite all --cycles 10 --report-json soak.json
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from datetime import datetime
from typing import List, Optional, Sequence

# 确保当前脚本目录在 sys.path 中，以便独立目录运行时也能 import uart_auto_test
_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if _SCRIPT_DIR not in sys.path:
    sys.path.insert(0, _SCRIPT_DIR)

import serial

from uart_auto_test import (
    DEFAULT_BUSINESS_PORT,
    DEFAULT_RX_DEBUG_PORT,
    DEFAULT_TX_DEBUG_PORT,
    DebugMonitor,
    TestFailure,
    TestResult,
    Ws63AutoTester,
    apply_config_defaults,
    load_config_file,
    resolve_suites,
    run_named_suite,
    setup_file_logging,
    windows_serial_hint,
)


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="WS63 transmitter UART stress test")
    parser.add_argument(
        "port",
        nargs="?",
        default=DEFAULT_BUSINESS_PORT,
        help=f"Business UART port connected to transmitter UART1 (default: {DEFAULT_BUSINESS_PORT})",
    )
    parser.add_argument("--baud", type=int, default=115200, help="Business UART baud rate")
    parser.add_argument(
        "--tx-debug-port",
        "--debug-port",
        dest="tx_debug_port",
        default=DEFAULT_TX_DEBUG_PORT,
        help=f"Transmitter UART0 debug port (default: {DEFAULT_TX_DEBUG_PORT})",
    )
    parser.add_argument(
        "--rx-debug-port",
        default=DEFAULT_RX_DEBUG_PORT,
        help=f"Receiver UART0 debug port (default: {DEFAULT_RX_DEBUG_PORT})",
    )
    parser.add_argument("--no-tx-debug", action="store_true", help="Disable transmitter debug port monitoring")
    parser.add_argument("--no-rx-debug", action="store_true", help="Disable receiver debug port monitoring")
    parser.add_argument("--debug-baud", type=int, default=115200, help="Debug UART baud rate")
    parser.add_argument(
        "--suite",
        choices=("smoke", "square", "repeat", "all"),
        default="repeat",
        help="Test suite to loop",
    )
    parser.add_argument("--rounds", type=int, default=20, help="Repeat count used by the repeat suite")
    parser.add_argument("--cycles", type=int, default=20, help="How many cycles to run")
    parser.add_argument("--cycle-delay", type=float, default=0.2, help="Delay between cycles in seconds")
    parser.add_argument("--ready-timeout", type=float, default=20.0, help="Timeout waiting for link ready")
    parser.add_argument("--ready-grace", type=float, default=8.0, help="Fallback wait time without debug ports")
    parser.add_argument("--idle-timeout", type=float, default=20.0, help="Timeout waiting for motion to return Idle")
    parser.add_argument("--keep-going", action="store_true", help="Continue running after a failed cycle")
    parser.add_argument("--report-json", help="Write a machine-readable stress report to JSON")
    parser.add_argument("--verbose", action="store_true", help="Print every received line")
    parser.add_argument(
        "--log-dir",
        default=None,
        help="Directory to write timestamped log files (default: no file logging)",
    )
    parser.add_argument(
        "--config",
        default=None,
        help="Path to test_config.json (default: auto-detect in script directory)",
    )
    return parser


def write_report(
    path: str,
    args: argparse.Namespace,
    results: Sequence[dict],
    exit_code: int,
    failure_detail: Optional[str],
    recent_debug_lines: dict[str, List[str]],
) -> None:
    payload = {
        "timestamp": datetime.now().astimezone().isoformat(timespec="seconds"),
        "exit_code": exit_code,
        "passed": exit_code == 0,
        "failure_detail": failure_detail,
        "port": args.port,
        "baud": args.baud,
        "suite": args.suite,
        "resolved_suites": resolve_suites(args.suite),
        "rounds": args.rounds,
        "cycles": args.cycles,
        "cycle_delay": args.cycle_delay,
        "results": list(results),
        "recent_debug_lines": recent_debug_lines,
    }
    with open(path, "w", encoding="utf-8") as fp:
        json.dump(payload, fp, indent=2, ensure_ascii=False)
        fp.write("\n")


def print_recent_debug_lines(debug_monitors: Sequence[DebugMonitor], stream: object = sys.stderr) -> None:
    for monitor in debug_monitors:
        print(f"Recent {monitor.label} lines ({monitor.port}):", file=stream)
        recent = monitor.recent_lines()[-20:]
        if not recent:
            print("<no recent lines>", file=stream)
            continue
        for line in recent:
            print(line, file=stream)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_arg_parser().parse_args(argv)

    # 加载配置文件
    cfg = load_config_file(getattr(args, "config", None))
    apply_config_defaults(args, cfg)

    # 文件日志
    if args.log_dir:
        log_path = setup_file_logging(args.log_dir, prefix="ws63_stress")
        print(f"Logging to: {log_path}")

    debug_monitors: List[DebugMonitor] = []
    tester: Optional[Ws63AutoTester] = None
    suite_names = resolve_suites(args.suite)
    results: List[dict] = []
    exit_code = 0
    failure_detail: Optional[str] = None
    current_cycle = 0
    current_suite: Optional[str] = None

    try:
        if not args.no_tx_debug and args.tx_debug_port:
            print(f"Opening transmitter debug UART: {args.tx_debug_port} @ {args.debug_baud}")
            debug_monitors.append(DebugMonitor(args.tx_debug_port, args.debug_baud, "tx-debug", verbose=args.verbose))

        if not args.no_rx_debug and args.rx_debug_port:
            print(f"Opening receiver debug UART: {args.rx_debug_port} @ {args.debug_baud}")
            debug_monitors.append(DebugMonitor(args.rx_debug_port, args.debug_baud, "rx-debug", verbose=args.verbose))

        print(f"Opening business UART: {args.port} @ {args.baud}")
        tester = Ws63AutoTester(args.port, args.baud, verbose=args.verbose, debug_monitors=debug_monitors)

        print("Waiting for link ready...")
        tester.wait_ready(args.ready_timeout, args.ready_grace)
        print("Link ready, start stress cycles.")

        for cycle in range(1, args.cycles + 1):
            current_cycle = cycle
            print(f"Cycle {cycle}/{args.cycles}")
            cycle_failed = False
            for suite in suite_names:
                current_suite = suite
                start = time.monotonic()
                try:
                    result: TestResult = run_named_suite(tester, suite, args.rounds, args.idle_timeout)
                    results.append(
                        {
                            "cycle": cycle,
                            "suite": suite,
                            "passed": True,
                            "detail": result.detail,
                            "elapsed_s": result.elapsed_s,
                        }
                    )
                    print(f"[PASS] cycle={cycle} suite={suite} detail={result.detail} ({result.elapsed_s:.2f}s)")
                except TestFailure as exc:
                    elapsed_s = time.monotonic() - start
                    failure_detail = f"cycle={cycle} suite={suite}: {exc}"
                    results.append(
                        {
                            "cycle": cycle,
                            "suite": suite,
                            "passed": False,
                            "detail": str(exc),
                            "elapsed_s": elapsed_s,
                        }
                    )
                    print(f"[FAIL] {failure_detail}", file=sys.stderr)
                    print_recent_debug_lines(debug_monitors)
                    cycle_failed = True
                    exit_code = 1
                    if not args.keep_going:
                        break
            if cycle_failed and not args.keep_going:
                break
            if cycle < args.cycles and args.cycle_delay > 0:
                time.sleep(args.cycle_delay)
            current_suite = None

    except KeyboardInterrupt:
        failure_detail = f"interrupted at cycle={current_cycle} suite={current_suite or '<idle>'}"
        exit_code = 130
        print(f"[INTERRUPTED] {failure_detail}", file=sys.stderr)
        print_recent_debug_lines(debug_monitors)
    except (TestFailure, serial.SerialException, OSError) as exc:
        failure_detail = str(exc)
        exit_code = 1
        if isinstance(exc, (serial.SerialException, OSError)) and sys.platform == "win32":
            print(windows_serial_hint(args.port, exc), file=sys.stderr)
        else:
            print(f"[FAIL] {failure_detail}", file=sys.stderr)
        print_recent_debug_lines(debug_monitors)
    finally:
        recent_debug_lines = {
            f"{monitor.label}@{monitor.port}": monitor.recent_lines()[-20:] for monitor in debug_monitors
        }
        if tester is not None:
            tester.close()
        for monitor in debug_monitors:
            monitor.close()
        if args.report_json:
            write_report(args.report_json, args, results, exit_code, failure_detail, recent_debug_lines)

    total = len(results)
    passed = sum(1 for result in results if result["passed"])
    failed = total - passed
    print(f"Summary: total={total} passed={passed} failed={failed}")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
