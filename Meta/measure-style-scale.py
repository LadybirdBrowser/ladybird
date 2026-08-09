#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
# SPDX-License-Identifier: BSD-2-Clause

"""Measure one fixed StyleEngine action and reaction across document sizes."""

import argparse
import json
import math
import socket
import statistics
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

from pathlib import Path

DEFAULT_WEBDRIVER = "Build/release/bin/Ladybird.app/Contents/MacOS/WebDriver"

GRAPH_COUNTERS = [
    "normalizedUniqueKeys",
    "routedEntryPoints",
    "candidateChecks",
    "localFeatureTests",
    "combinatorSteps",
    "structuralTests",
    "coldNodesEvaluated",
    "selectorTruthAdditions",
    "selectorTruthRemovals",
    "selectorTruthRefreshes",
    "matchAnswerChanges",
    "matchAnswerUpqueries",
    "retainedMatchAnswerReuses",
    "retainedMatchAnswerRefusals",
    "retainedMatchAnswerPatches",
    "retainedMatchAnswerDeltaPatches",
    "retainedMatchAnswerFilteredPatches",
    "retainedMatchAnswerPatchMisses",
    "retainedPatchesUnattributed",
    "retainedPatchesPoisoned",
    "retainedPatchesCoarseCovered",
    "tier3BenefitEvictions",
    "tier3AdmissionRetries",
    "remainingPostingRowsCopied",
    "remainingPostingRowsInspected",
    "preparedMatchingBatchCompletenessRowsInspected",
    "preparedMatchingBatchRowsCloned",
    "invalidatedStyleNodes",
    "plannedNodesWithDirectAction",
    "plannedNodesWithSignedDelta",
    "plannedNodesWithOutputChange",
    "plannedNodesWithUpquery",
    "plannedNodesUnattributed",
]

STYLE_COUNTERS = [
    "styleEngineReactionElements",
    "styleEnginePublishedReactions",
    "elementStyleRecomputations",
    "elementComputedStyleChanges",
    "committedStyleObserverConsequences",
]

BENCHMARK_SCRIPT = r"""
const documentSize = arguments[0];
const cycles = arguments[1];
const warmupCycles = arguments[2];
const graphCounterNames = arguments[3];
const styleCounterNames = arguments[4];

document.body.replaceChildren();
const style = document.createElement('style');
style.textContent = '#target.active { color: red; }';
document.head.appendChild(style);
const root = document.createElement('div');
const target = document.createElement('div');
target.id = 'target';
root.appendChild(target);
const fragment = document.createDocumentFragment();
for (let index = 1; index < documentSize; ++index) {
    const filler = document.createElement('div');
    filler.className = 'unrelated';
    fragment.appendChild(filler);
}
root.appendChild(fragment);
document.body.appendChild(root);
target.offsetWidth;

function difference(after, before, names) {
    return Object.fromEntries(names.map(name => [name, after[name] - before[name]]));
}

function toggle(active) {
    const graphBefore = internals.styleEngineCounters();
    const styleBefore = internals.getStyleInvalidationCounters();
    const wallBefore = performance.now();
    target.classList.toggle('active', active);
    target.offsetWidth;
    const wallMicroseconds = (performance.now() - wallBefore) * 1000;
    const graphAfter = internals.styleEngineCounters();
    const styleAfter = internals.getStyleInvalidationCounters();
    return {
        wallMicroseconds,
        styleUpdateMicroseconds: styleAfter.styleUpdateMicroseconds - styleBefore.styleUpdateMicroseconds,
        graph: difference(graphAfter, graphBefore, graphCounterNames),
        allGraph: difference(graphAfter, graphBefore, Object.keys(graphAfter)),
        style: difference(styleAfter, styleBefore, styleCounterNames),
        allStyle: difference(styleAfter, styleBefore, Object.keys(styleAfter)),
    };
}

for (let cycle = 0; cycle < warmupCycles; ++cycle) {
    toggle(true);
    toggle(false);
}

const samples = [];
for (let cycle = 0; cycle < cycles; ++cycle) {
    samples.push(toggle(true));
    samples.push(toggle(false));
}
return JSON.stringify(samples);
"""


def request_json(base_url, method, path, body=None, timeout=600):
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(
        base_url + path,
        data=data,
        headers={"Content-Type": "application/json"},
        method=method,
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.load(response)


def unused_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def wait_for_webdriver(base_url, process):
    for _ in range(200):
        if process.poll() is not None:
            raise RuntimeError("WebDriver exited before accepting connections")
        try:
            request_json(base_url, "GET", "/status", timeout=1)
            return
        except (OSError, urllib.error.URLError):
            time.sleep(0.05)
    raise RuntimeError("WebDriver did not accept connections within 10 seconds")


def stop_process(process):
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def run_iteration(webdriver, document_size, cycles, warmup_cycles):
    port = unused_port()
    base_url = f"http://127.0.0.1:{port}"
    with tempfile.TemporaryDirectory(prefix="ladybird-stylegraph-scale-") as profiles_directory:
        process = subprocess.Popen(
            [
                webdriver,
                "--headless",
                "--expose-internals-object",
                "--profiles-directory",
                profiles_directory,
                "--port",
                str(port),
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        session = None
        try:
            wait_for_webdriver(base_url, process)
            session = request_json(
                base_url,
                "POST",
                "/session",
                {"capabilities": {"alwaysMatch": {}}},
            )["value"]["sessionId"]
            request_json(
                base_url,
                "POST",
                f"/session/{session}/timeouts",
                {"script": 600_000},
            )
            value = request_json(
                base_url,
                "POST",
                f"/session/{session}/execute/sync",
                {
                    "script": BENCHMARK_SCRIPT,
                    "args": [document_size, cycles, warmup_cycles, GRAPH_COUNTERS, STYLE_COUNTERS],
                },
            )["value"]
            return json.loads(value)
        finally:
            if session is not None:
                try:
                    request_json(base_url, "DELETE", f"/session/{session}", timeout=10)
                except (OSError, urllib.error.URLError):
                    pass
            stop_process(process)


def percentile(values, percentile_value):
    ordered = sorted(values)
    index = min(len(ordered) - 1, math.ceil((len(ordered) - 1) * percentile_value))
    return ordered[index]


def summarize(values):
    return {
        "minimum": min(values),
        "median": statistics.median(values),
        "p95": percentile(values, 0.95),
        "maximum": max(values),
    }


def summarize_size(iterations):
    samples = [sample for iteration in iterations for sample in iteration]
    graph = {name: sorted(set(sample["graph"][name] for sample in samples)) for name in GRAPH_COUNTERS}
    style = {name: sorted(set(sample["style"][name] for sample in samples)) for name in STYLE_COUNTERS}
    nonzero_graph = {
        name: values
        for name in samples[0]["allGraph"]
        if (values := sorted(set(sample["allGraph"][name] for sample in samples))) != [0]
    }
    nonzero_style = {
        name: values
        for name in samples[0]["allStyle"]
        if (values := sorted(set(sample["allStyle"][name] for sample in samples))) != [0]
    }
    return {
        "wall_microseconds": summarize([sample["wallMicroseconds"] for sample in samples]),
        "style_update_microseconds": summarize([sample["styleUpdateMicroseconds"] for sample in samples]),
        "graph_counter_values": graph,
        "nonzero_graph_counter_values": nonzero_graph,
        "style_counter_values": style,
        "nonzero_style_counter_values": nonzero_style,
    }


def parse_document_sizes(value):
    try:
        sizes = [int(item) for item in value.split(",")]
    except ValueError as error:
        raise argparse.ArgumentTypeError("document sizes must be comma-separated integers") from error
    if not sizes or any(size < 1 for size in sizes):
        raise argparse.ArgumentTypeError("document sizes must all be positive")
    if sizes != sorted(set(sizes)):
        raise argparse.ArgumentTypeError("document sizes must be unique and increasing")
    return sizes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--webdriver", default=DEFAULT_WEBDRIVER, help="WebDriver executable")
    parser.add_argument(
        "--document-sizes",
        type=parse_document_sizes,
        default=parse_document_sizes("4096,16384,65536"),
        help="comma-separated increasing element counts",
    )
    parser.add_argument("--iterations", type=int, default=2, help="fresh WebDriver processes per size")
    parser.add_argument("--cycles", type=int, default=100, help="measured add/remove pairs per process")
    parser.add_argument("--warmup-cycles", type=int, default=10, help="unmeasured add/remove pairs per process")
    parser.add_argument("--output", type=Path, help="also write the JSON result to this path")
    args = parser.parse_args()

    if args.iterations < 1 or args.cycles < 1 or args.warmup_cycles < 0:
        parser.error("iterations and cycles must be positive, and warmup cycles must be non-negative")
    webdriver = str(Path(args.webdriver).resolve())
    if not Path(webdriver).is_file():
        parser.error(f"WebDriver executable not found: {webdriver}")

    by_size = {}
    for document_size in args.document_sizes:
        iterations = []
        for iteration in range(args.iterations):
            print(
                f"Running size {document_size}, iteration {iteration + 1}/{args.iterations}...",
                flush=True,
            )
            iterations.append(run_iteration(webdriver, document_size, args.cycles, args.warmup_cycles))
        by_size[str(document_size)] = summarize_size(iterations)

    counter_signatures = {
        size: {
            "graph": summary["graph_counter_values"],
            "style": summary["style_counter_values"],
        }
        for size, summary in by_size.items()
    }
    signatures = list(counter_signatures.values())
    result = {
        "webdriver": webdriver,
        "document_sizes": args.document_sizes,
        "iterations_per_size": args.iterations,
        "cycles": args.cycles,
        "physical_counters_are_invariant": all(signature == signatures[0] for signature in signatures),
        "by_size": by_size,
    }
    encoded = json.dumps(result, indent=2)
    print(encoded)
    if args.output:
        args.output.write_text(encoded + "\n")


if __name__ == "__main__":
    main()
