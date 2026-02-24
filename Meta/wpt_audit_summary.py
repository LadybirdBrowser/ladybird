#!/usr/bin/env python3

import argparse
import re
import sys

from dataclasses import dataclass
from dataclasses import field
from typing import Iterable
from typing import List
from typing import Optional
from typing import Tuple


@dataclass
class TaskResult:
    name: str
    failures: List[str] = field(default_factory=list)
    passes: int = 0


_RUN_TOKEN_RE = re.compile(r"(?=(?:^|\s)(Pass|Asserts)\s+run(?:Pass|Fail)[^\s]*)")
_TASK_NAME_RE = re.compile(r"\[([^\]]+)]")


def _read_text(path: Optional[str]) -> str:
    if path is None or path == "-":
        return sys.stdin.read()
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def _iter_chunks(text: str) -> Iterable[str]:
    # The captured output is often a single long line with embedded stack traces.
    # Split on token boundaries instead of relying on newlines.
    parts = _RUN_TOKEN_RE.split(text)
    if len(parts) <= 1:
        yield text
        return

    # split() keeps delimiters due to capturing group; reconstitute.
    it = iter(parts)
    prefix = next(it, "")
    if prefix.strip():
        yield prefix

    while True:
        leader = next(it, None)
        if leader is None:
            break
        rest = next(it, "")
        yield (leader + rest).strip()


def _extract_task_name(chunk: str) -> Optional[str]:
    # Typical patterns:
    #   Asserts runPass> [Linear + Expo] Different events at same time
    #   Asserts runFail< [Multiple linear ramps at the same time] 2 out of 6 assertions were failed.
    m = _TASK_NAME_RE.search(chunk)
    if not m:
        return None
    return m.group(1).strip()


def summarize(text: str) -> Tuple[List[TaskResult], List[str]]:
    tasks: List[TaskResult] = []
    current: Optional[TaskResult] = None
    misc_failures: List[str] = []

    for chunk in _iter_chunks(text):
        if not chunk:
            continue

        # If the chunk introduces a task name, update the active task.
        task_name = _extract_task_name(chunk)
        if task_name is not None:
            # Start a new TaskResult when we encounter a new header marker.
            # This keeps ordering stable even if the task name repeats.
            current = TaskResult(name=task_name)
            tasks.append(current)

        # Record pass/fail lines. We only retain concise fail descriptions.
        if "runPass" in chunk:
            if current is not None:
                current.passes += 1
            continue

        if "runFail" in chunk or "runFailX" in chunk or "runFail<" in chunk or "runFail#" in chunk:
            # Grab the first sentence-ish error message.
            msg = chunk
            # Drop stack traces to keep it readable.
            msg = msg.split("assert_wrapper", 1)[0]
            msg = msg.split("at <unknown>", 1)[0]
            msg = msg.replace("\n", " ").strip()
            msg = re.sub(r"\s+", " ", msg)

            if current is not None:
                current.failures.append(msg)
            else:
                misc_failures.append(msg)

    # Coalesce consecutive duplicate task names (common when the log repeats headers).
    coalesced: List[TaskResult] = []
    for t in tasks:
        if coalesced and coalesced[-1].name == t.name:
            coalesced[-1].passes += t.passes
            coalesced[-1].failures.extend(t.failures)
        else:
            coalesced.append(t)

    # Filter empty tasks.
    coalesced = [t for t in coalesced if t.failures or t.passes]
    return coalesced, misc_failures


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Summarize WPT WebAudio audit.js/testharness output captured from lbtest-gpt or test-web.",
    )
    parser.add_argument("path", nargs="?", default="-", help="Path to captured output (or '-' for stdin)")
    parser.add_argument("--max-failures", type=int, default=10, help="Max failure lines shown per task")
    args = parser.parse_args(argv)

    text = _read_text(args.path)
    tasks, misc = summarize(text)

    total_fail_lines = sum(len(t.failures) for t in tasks) + len(misc)
    print(f"Tasks seen: {len(tasks)}")
    print(f"Failure entries: {total_fail_lines}")

    for task in tasks:
        print("\n== ", task.name)
        if task.failures:
            shown = task.failures[: args.max_failures]
            for f in shown:
                print("- ", f)
            if len(task.failures) > len(shown):
                print(f"  (… {len(task.failures) - len(shown)} more …)")
        else:
            print("- (no failures captured)")

    if misc:
        print("\n== Unattributed")
        for f in misc[: args.max_failures]:
            print("- ", f)
        if len(misc) > args.max_failures:
            print(f"  (… {len(misc) - args.max_failures} more …)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
