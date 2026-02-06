#!/usr/bin/env python3
"""Analyze dx9mt runtime logs and collapse duplicate call patterns."""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional


LOG_LINE_RE = re.compile(
    r"^\[(?P<time>\d\d:\d\d:\d\d)\]\s+dx9mt/(?P<tag>[^:]+):\s+(?P<msg>.*)$"
)
CALL_RE = re.compile(r"^(?P<name>[A-Za-z0-9_]+)(?:\s+(?P<args>.*))?$")
ARG_RE = re.compile(r"([A-Za-z0-9_]+)=([^\s]+)")


@dataclass
class ParsedEvent:
    time: str
    tag: str
    message: str
    call: Optional[str]
    args: dict[str, str]


def parse_line(line: str) -> Optional[ParsedEvent]:
    match = LOG_LINE_RE.match(line.rstrip("\n"))
    if not match:
        return None
    time = match.group("time")
    tag = match.group("tag")
    message = match.group("msg")
    call = None
    args: dict[str, str] = {}

    call_match = CALL_RE.match(message)
    if call_match:
        call = call_match.group("name")
        arg_blob = call_match.group("args") or ""
        for key, value in ARG_RE.findall(arg_blob):
            args[key] = value

    return ParsedEvent(time=time, tag=tag, message=message, call=call, args=args)


def normalize_message(msg: str) -> str:
    """Normalize volatile values to make duplicate grouping more useful."""
    msg = re.sub(r"\b0x[0-9a-fA-F]+\b", "<hex>", msg)
    msg = re.sub(r"\b[0-9a-fA-F]{8,}\b", "<addr>", msg)
    msg = re.sub(r"\s+", " ", msg).strip()
    return msg


def iter_events(lines: Iterable[str]) -> Iterable[ParsedEvent]:
    for line in lines:
        event = parse_line(line)
        if event is not None:
            yield event


def print_header(title: str) -> None:
    print()
    print(f"== {title} ==")


def summarize_log(events: list[ParsedEvent], top_n: int) -> None:
    by_tag = Counter(event.tag for event in events)
    by_call = Counter()
    normalized_messages = Counter()
    contiguous_runs: list[tuple[str, int]] = []

    prev_key = None
    run_length = 0

    for event in events:
        if event.call:
            by_call[f"{event.tag}:{event.call}"] += 1
        normalized = f"{event.tag}:{normalize_message(event.message)}"
        normalized_messages[normalized] += 1

        if normalized == prev_key:
            run_length += 1
        else:
            if prev_key is not None:
                contiguous_runs.append((prev_key, run_length))
            prev_key = normalized
            run_length = 1
    if prev_key is not None:
        contiguous_runs.append((prev_key, run_length))

    print_header("Overview")
    print(f"Parsed dx9mt lines: {len(events)}")
    print("By tag:")
    for tag, count in by_tag.most_common():
        print(f"  {tag:12s} {count}")

    print_header(f"Top {top_n} calls")
    for call_key, count in by_call.most_common(top_n):
        print(f"  {count:6d}  {call_key}")

    print_header(f"Top {top_n} duplicate messages (normalized)")
    for key, count in normalized_messages.most_common(top_n):
        print(f"  {count:6d}  {key}")

    print_header(f"Top {top_n} contiguous repeat runs")
    for key, length in sorted(contiguous_runs, key=lambda item: item[1], reverse=True)[
        :top_n
    ]:
        if length <= 1:
            continue
        print(f"  {length:6d}x {key}")


def summarize_multisample_sweeps(events: list[ParsedEvent], top_n: int) -> None:
    groups: dict[tuple[str, str, str, str], list[int]] = defaultdict(list)

    for event in events:
        if event.call != "CheckDeviceMultiSampleType":
            continue
        adapter = event.args.get("adapter")
        dev_type = event.args.get("type")
        fmt = event.args.get("fmt")
        windowed = event.args.get("windowed")
        ms_value = event.args.get("ms")
        if None in (adapter, dev_type, fmt, windowed, ms_value):
            continue
        try:
            ms = int(ms_value)
        except ValueError:
            continue
        groups[(adapter, dev_type, fmt, windowed)].append(ms)

    if not groups:
        print_header("CheckDeviceMultiSampleType sweeps")
        print("No multisample sweep calls found.")
        return

    ranked = sorted(groups.items(), key=lambda item: len(item[1]), reverse=True)

    print_header(
        f"Top {min(top_n, len(ranked))} CheckDeviceMultiSampleType sweep groups"
    )
    for (adapter, dev_type, fmt, windowed), values in ranked[:top_n]:
        counts = Counter(values)
        distinct = sorted(counts)
        missing = []
        if distinct:
            for candidate in range(distinct[0], distinct[-1] + 1):
                if candidate not in counts:
                    missing.append(candidate)
        range_str = (
            f"{distinct[0]}..{distinct[-1]}" if distinct else "(none)"
        )
        missing_str = ",".join(str(v) for v in missing[:10]) or "-"
        if len(missing) > 10:
            missing_str += ",..."
        print(
            "  "
            f"adapter={adapter} type={dev_type} fmt={fmt} windowed={windowed} "
            f"calls={len(values)} unique_ms={len(distinct)} ms_range={range_str} "
            f"missing={missing_str}"
        )


def find_last_call_before_detach(events: list[ParsedEvent]) -> None:
    last_call: Optional[ParsedEvent] = None
    detach: Optional[ParsedEvent] = None

    for event in events:
        if event.tag == "dll" and event.message.startswith("PROCESS_DETACH"):
            detach = event
            break
        if event.call:
            last_call = event

    print_header("Detach context")
    if detach is None:
        print("No PROCESS_DETACH found.")
        return
    print(f"First detach at {detach.time}")
    if last_call is None:
        print("No call-level event found before detach.")
        return
    print(
        f"Last call before first detach: {last_call.time} {last_call.tag}:{last_call.message}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Analyze dx9mt runtime log and collapse duplicate patterns."
    )
    parser.add_argument(
        "log",
        nargs="?",
        default="/tmp/dx9mt_runtime.log",
        help="Path to runtime log (default: /tmp/dx9mt_runtime.log)",
    )
    parser.add_argument(
        "--top",
        type=int,
        default=20,
        help="Number of rows to show in top sections (default: 20)",
    )
    args = parser.parse_args()

    path = Path(args.log)
    if not path.is_file():
        print(f"error: log file not found: {path}", file=sys.stderr)
        return 1

    with path.open("r", encoding="utf-8", errors="replace") as handle:
        events = list(iter_events(handle))

    if not events:
        print(f"No parseable dx9mt lines found in: {path}")
        return 1

    print(f"Log: {path}")
    summarize_log(events, top_n=args.top)
    summarize_multisample_sweeps(events, top_n=args.top)
    find_last_call_before_detach(events)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
