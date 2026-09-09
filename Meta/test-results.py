#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import json
import re
import shlex
import sys

from html.parser import HTMLParser
from pathlib import Path


class LogText(HTMLParser):
    def __init__(self):
        super().__init__()
        self.in_pre = False
        self.parts = []

    def handle_starttag(self, tag, attrs):
        if tag == "pre":
            self.in_pre = True

    def handle_endtag(self, tag):
        if tag == "pre":
            self.in_pre = False

    def handle_data(self, data):
        if self.in_pre:
            self.parts.append(data)


def read_results(path):
    source = path.read_text(encoding="utf-8")
    match = re.fullmatch(r"\s*const RESULTS_DATA = (\{.*\});\s*", source, re.DOTALL)
    if not match:
        raise ValueError(f"{path}: expected a test-web results.js file")
    return json.loads(match[1])


def artifact_path(directory, name):
    path = (directory / name).resolve()
    if not path.is_relative_to(directory):
        raise ValueError(f"Result artifact is outside the results directory: {name}")
    return path


def show_artifact(path, details):
    if not path.is_file():
        return
    print(f"  {path}")
    if not details or path.suffix not in (".txt", ".html"):
        return
    content = path.read_text(encoding="utf-8", errors="replace")
    if path.suffix == ".html":
        parser = LogText()
        parser.feed(content)
        content = "".join(parser.parts)
    if content:
        print(content, end="" if content.endswith("\n") else "\n")


def rerun_filter(name, test_root):
    name = re.sub(r"^run-\d+/", "", name)
    if (test_root / name).is_file():
        return name
    for separator in re.finditer("@", name):
        if (test_root / name[: separator.start()]).is_file():
            return name[: separator.start()] + "?" + name[separator.end() :]
    return None


def show_results(directory, results, name_filter, details, test_root):
    summary = results["summary"]
    print(
        f"Total: {summary['total']}, failed: {summary['fail']}, "
        f"timed out: {summary['timeout']}, crashed: {summary['crashed']}, skipped: {summary['skipped']}"
    )
    if invocation := results.get("invocationCommandLine"):
        print(f"Recorded invocation: {invocation}")
    print(f"Report: {directory / 'index.html'}")

    for test in results["tests"]:
        if test["result"] in ("Pass", "Skipped") or name_filter not in test["name"]:
            continue
        name = test["name"]
        print(f"\n{test['result']}: {name} ({test['mode']})")
        if "pixelErrors" in test:
            print(f"  Pixel errors: {test['pixelErrors']}, maximum channel difference: {test['maxChannelDiff']}")

        # Results record filesystem-safe names, so use the source tree to
        # distinguish a variant separator from a literal '@' in a filename.
        test_filter = rerun_filter(name, test_root)
        if test_filter is not None:
            print(f"  Rerun from the build directory: ./bin/test-web -f {shlex.quote(test_filter)}")
        else:
            print("  Test source not found; use --test-root to enable a rerun suggestion.")
        for suffix in (".diff.txt", ".logs.html"):
            show_artifact(artifact_path(directory, name + suffix), details)
        for suffix in (".actual.txt", ".expected.txt", ".actual.png", ".expected.png", ".diff.png"):
            show_artifact(artifact_path(directory, name + suffix), False)

    helper_logs = directory / "helper-process-logs.html"
    if helper_logs.is_file():
        print("\nHelper process logs (whole run):")
        show_artifact(helper_logs, details)


def main():
    parser = argparse.ArgumentParser(
        description="Summarize an existing test-web results directory without rerunning tests."
    )
    parser.add_argument("results", type=Path, help="Results directory or results.js file")
    parser.add_argument("-f", "--filter", default="", help="Only show failures whose artifact names contain this text")
    parser.add_argument("--details", action="store_true", help="Also print text diffs and captured logs as plain text")
    parser.add_argument(
        "--test-root",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "Tests/LibWeb",
        help="Source test root used to reconstruct query variants (default: this checkout's Tests/LibWeb)",
    )
    args = parser.parse_args()

    path = args.results.resolve()
    if path.is_dir():
        path /= "results.js"
    try:
        show_results(path.parent, read_results(path), args.filter, args.details, args.test_root)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Unable to read test results: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
