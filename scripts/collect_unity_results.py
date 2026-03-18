#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable
import xml.etree.ElementTree as ET

import serial


BOOT_PROMPT = "Press ENTER to see the list of tests."
MENU_PROMPT = "Enter test for running."
NEXT_PROMPT = "Enter next test, or 'enter' to see menu"
SUMMARY_RE = re.compile(r"(?P<tests>\d+)\s+Tests\s+(?P<failures>\d+)\s+Failures\s+(?P<ignored>\d+)\s+Ignored")
CASE_RE = re.compile(
    r"^(?P<file>[^:\r\n]+):(?P<line>\d+):(?P<name>.+?):(?P<status>PASS|FAIL|IGNORE)(?::(?P<message>.*))?$"
)


@dataclass
class UnityCase:
    file: str
    line: int
    name: str
    status: str
    message: str


@dataclass
class UnityRun:
    selector: str
    tests: int
    failures: int
    ignored: int
    cases: list[UnityCase]
    raw_log: str


class SerialTimeout(RuntimeError):
    pass


class UnitySerialCollector:
    def __init__(self, port: str, baud: int, read_timeout: float = 0.2) -> None:
        self.serial = serial.Serial(port=port, baudrate=baud, timeout=read_timeout)
        self.serial.dtr = False
        self.serial.rts = False
        self.full_log = ""

    def close(self) -> None:
        self.serial.close()

    def _read_chunk(self) -> str:
        chunk = self.serial.read(4096)
        if not chunk:
            return ""
        decoded = chunk.decode("utf-8", errors="replace")
        self.full_log += decoded
        return decoded

    def _wait_for_any(self, markers: Iterable[str], timeout_s: float) -> str:
        deadline = time.monotonic() + timeout_s
        markers = tuple(markers)

        while time.monotonic() < deadline:
            self._read_chunk()
            for marker in markers:
                if marker in self.full_log:
                    return marker
            time.sleep(0.05)

        raise SerialTimeout(f"Timeout waiting for any marker: {markers}")

    def _wait_for_marker_after(self, marker: str, start_index: int, timeout_s: float) -> int:
        deadline = time.monotonic() + timeout_s

        while time.monotonic() < deadline:
            self._read_chunk()
            index = self.full_log.find(marker, start_index)
            if index >= 0:
                return index
            time.sleep(0.05)

        raise SerialTimeout(f"Timeout waiting for marker: {marker}")

    def _wait_for_summary_after(self, start_index: int, timeout_s: float) -> re.Match[str]:
        deadline = time.monotonic() + timeout_s

        while time.monotonic() < deadline:
            self._read_chunk()
            match = SUMMARY_RE.search(self.full_log, start_index)
            if match:
                return match
            time.sleep(0.05)

        raise SerialTimeout("Timeout waiting for Unity summary line")

    def write_line(self, text: str) -> None:
        self.serial.write(text.encode("utf-8"))
        self.serial.flush()

    def enter_menu(self, startup_timeout_s: float, menu_timeout_s: float) -> None:
        marker = self._wait_for_any((BOOT_PROMPT, MENU_PROMPT, NEXT_PROMPT), startup_timeout_s)

        if marker != MENU_PROMPT:
            self.write_line("\n")
            menu_start = len(self.full_log)
            self._wait_for_marker_after(MENU_PROMPT, menu_start, menu_timeout_s)

    def run_selector(self, selector: str, run_timeout_s: float) -> UnityRun:
        start = len(self.full_log)
        self.write_line(f"{selector}\n")
        summary_match = self._wait_for_summary_after(start, run_timeout_s)
        self._wait_for_marker_after(NEXT_PROMPT, summary_match.end(), run_timeout_s)
        raw = self.full_log[start:]
        return UnityRun(
            selector=selector,
            tests=int(summary_match.group("tests")),
            failures=int(summary_match.group("failures")),
            ignored=int(summary_match.group("ignored")),
            cases=parse_cases(raw),
            raw_log=raw,
        )


def parse_cases(text: str) -> list[UnityCase]:
    cases: list[UnityCase] = []

    for line in text.splitlines():
        match = CASE_RE.match(line.strip())
        if not match:
            continue
        cases.append(
            UnityCase(
                file=match.group("file"),
                line=int(match.group("line")),
                name=match.group("name"),
                status=match.group("status"),
                message=match.group("message") or "",
            )
        )

    return cases


def write_summary_json(path: Path, runs: list[UnityRun]) -> None:
    payload = {
        "selectors": [
            {
                "selector": run.selector,
                "tests": run.tests,
                "failures": run.failures,
                "ignored": run.ignored,
                "cases": [
                    {
                        "file": case.file,
                        "line": case.line,
                        "name": case.name,
                        "status": case.status,
                        "message": case.message,
                    }
                    for case in run.cases
                ],
            }
            for run in runs
        ],
        "total_tests": sum(run.tests for run in runs),
        "total_failures": sum(run.failures for run in runs),
        "total_ignored": sum(run.ignored for run in runs),
    }
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")


def write_junit_xml(path: Path, runs: list[UnityRun]) -> None:
    root = ET.Element(
        "testsuites",
        tests=str(sum(run.tests for run in runs)),
        failures=str(sum(run.failures for run in runs)),
        skipped=str(sum(run.ignored for run in runs)),
    )

    for run in runs:
        suite = ET.SubElement(
            root,
            "testsuite",
            name=run.selector,
            tests=str(run.tests),
            failures=str(run.failures),
            skipped=str(run.ignored),
        )
        for case in run.cases:
            testcase = ET.SubElement(
                suite,
                "testcase",
                classname=Path(case.file).stem,
                name=case.name,
                file=case.file,
                line=str(case.line),
            )
            if case.status == "FAIL":
                failure = ET.SubElement(testcase, "failure", message=case.message or "Unity test failed")
                failure.text = case.message or ""
            elif case.status == "IGNORE":
                skipped = ET.SubElement(testcase, "skipped", message=case.message or "Unity test ignored")
                skipped.text = case.message or ""

    ET.indent(root)
    ET.ElementTree(root).write(path, encoding="utf-8", xml_declaration=True)


def write_markdown_summary(path: Path, runs: list[UnityRun]) -> None:
    lines = [
        "# EmbedClaw Unit Test Summary",
        "",
        "| Selector | Tests | Failures | Ignored |",
        "| --- | ---: | ---: | ---: |",
    ]

    for run in runs:
        lines.append(f"| `{run.selector}` | {run.tests} | {run.failures} | {run.ignored} |")

    lines.extend(
        [
            "",
            f"- Total tests: `{sum(run.tests for run in runs)}`",
            f"- Total failures: `{sum(run.failures for run in runs)}`",
            f"- Total ignored: `{sum(run.ignored for run in runs)}`",
        ]
    )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Drive Unity menu over serial and collect results.")
    parser.add_argument("--port", required=True, help="Serial port, for example /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    parser.add_argument(
        "--selector",
        action="append",
        dest="selectors",
        help="Unity selector such as [embed_claw] or [embed_claw][tools]",
    )
    parser.add_argument("--output-dir", default="build/unit-test-results", help="Directory for collected artifacts")
    parser.add_argument("--startup-timeout", type=float, default=30.0, help="Seconds to wait for Unity boot prompt")
    parser.add_argument("--menu-timeout", type=float, default=10.0, help="Seconds to wait for Unity menu prompt")
    parser.add_argument("--run-timeout", type=float, default=120.0, help="Seconds to wait for each selector run")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    selectors = args.selectors or ["[embed_claw]"]
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    collector = UnitySerialCollector(args.port, args.baud)
    runs: list[UnityRun] = []

    try:
        collector.enter_menu(args.startup_timeout, args.menu_timeout)
        for selector in selectors:
            runs.append(collector.run_selector(selector, args.run_timeout))
    except SerialTimeout as exc:
        (output_dir / "unity.log").write_text(collector.full_log, encoding="utf-8")
        print(str(exc), file=sys.stderr)
        return 2
    finally:
        raw_log = collector.full_log
        collector.close()

    (output_dir / "unity.log").write_text(raw_log, encoding="utf-8")
    write_summary_json(output_dir / "summary.json", runs)
    write_junit_xml(output_dir / "junit.xml", runs)
    write_markdown_summary(output_dir / "summary.md", runs)

    total_failures = sum(run.failures for run in runs)
    total_tests = sum(run.tests for run in runs)

    print(f"Collected {total_tests} Unity tests across {len(runs)} selector(s); failures={total_failures}")
    return 1 if total_failures > 0 or total_tests == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
