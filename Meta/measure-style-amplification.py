#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
# SPDX-License-Identifier: BSD-2-Clause

"""Profile StyleEngine action-to-reaction amplification in all StyleBench suites."""

import argparse
import json
import os
import socket
import statistics
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

from pathlib import Path

DEFAULT_WEBDRIVER = "Build/release/bin/Ladybird.app/Contents/MacOS/WebDriver"
DEFAULT_URL = "http://localhost:8000/resources/style-bench.html"
STYLE_BENCH_FLUSHES_PER_ITERATION = 26

PROFILE_SCRIPT = r"""
const suiteIndex = arguments[0];
const flushCount = arguments[1];
const mutationsPerKind = arguments[2];

function difference(after, before) {
    const result = {};
    for (const [name, value] of Object.entries(after)) {
        const delta = value - before[name];
        if (delta)
            result[name] = delta;
    }
    return result;
}

function snapshot() {
    return {
        graph: internals.styleEngineCounters(),
        style: internals.getStyleInvalidationCounters(),
        wall: performance.now(),
    };
}

function sample(before, after) {
    return {
        wallMicroseconds: (after.wall - before.wall) * 1000,
        graph: difference(after.graph, before.graph),
        style: difference(after.style, before.style),
    };
}

const kinds = arguments[3] ?? "all";
const configuration = StyleBench.predefinedConfigurations()[suiteIndex];
if (!configuration)
    throw new Error(`No StyleBench suite at index ${suiteIndex}`);

const setupBefore = snapshot();
const bench = createBenchmark(configuration);
bench.testRoot.offsetWidth;
const setup = sample(setupBefore, snapshot());

const flushes = [];
for (let flush = 0; flush < flushCount; ++flush) {
    const before = snapshot();
    if (kinds === "all" || kinds === "classes") {
        bench.addClasses(mutationsPerKind);
        bench.removeClasses(mutationsPerKind);
    }
    if (kinds === "all" || kinds === "leaves") {
        bench.addLeafElements(mutationsPerKind);
        bench.removeLeafElements(mutationsPerKind);
    }
    if (kinds === "all" || kinds === "attributes")
        bench.mutateAttributes(mutationsPerKind);
    bench.testRoot.offsetWidth;
    flushes.push(sample(before, snapshot()));
}

// One trailing nudge so set-style counters, published at transaction start, reflect the state
// after the measured flushes.
bench.addClasses(1);
bench.testRoot.offsetWidth;

return JSON.stringify({
    suite: configuration.name,
    setup,
    flushes,
    finalAbsolute: internals.styleEngineCounters(),
});
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


def run_profile(webdriver, url, suite_index, flushes, mutations_per_kind, kinds):
    port = unused_port()
    base_url = f"http://127.0.0.1:{port}"
    with tempfile.TemporaryDirectory(prefix="ladybird-stylegraph-amplification-") as profiles_directory:
        process = subprocess.Popen(
            [
                webdriver,
                "--headless",
                "--disable-sandbox",
                "--expose-internals-object",
                "--profiles-directory",
                profiles_directory,
                "--port",
                str(port),
            ],
            stdout=subprocess.DEVNULL,
            stderr=None if os.environ.get("STYLE_HARNESS_KEEP_STDERR") else subprocess.DEVNULL,
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
                {"script": 600_000, "pageLoad": 600_000},
            )
            request_json(base_url, "POST", f"/session/{session}/url", {"url": url})
            value = request_json(
                base_url,
                "POST",
                f"/session/{session}/execute/sync",
                {
                    "script": PROFILE_SCRIPT,
                    "args": [suite_index, flushes, mutations_per_kind, kinds],
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


def counter(sample, domain, name):
    return sample[domain].get(name, 0)


def counters(sample, domain, names):
    return sum(counter(sample, domain, name) for name in names)


def ratio(work, normalized_actions, reactions):
    denominator = normalized_actions + reactions
    return None if denominator == 0 else work / denominator


def stage_model(sample):
    normalized_actions = counter(sample, "graph", "normalizedUniqueKeys")
    selector_work = counters(
        sample,
        "graph",
        [
            "candidateChecks",
            "localFeatureTests",
            "stateTests",
            "combinatorSteps",
            "structuralTests",
            "relationalTests",
            "prefixCompoundsEvaluated",
            "prefixConvergenceNodes",
            "remainingPostingRowsInspected",
            "preparedMatchingBatchCompletenessRowsInspected",
        ],
    )
    selector_reactions = counters(
        sample,
        "graph",
        ["selectorTruthAdditions", "selectorTruthRemovals"],
    )
    match_work = counters(
        sample,
        "graph",
        [
            "coldNodesEvaluated",
            "ruleMatchesEmitted",
            "retainedMatchAnswerDeltaEntries",
            "preparedMatchingBatchRowsCloned",
        ],
    )
    match_reactions = counter(sample, "graph", "matchAnswerChanges")
    cascade_work = counter(sample, "graph", "cascadeMatchesBeforeCompaction")
    cascade_reactions = counter(sample, "graph", "cascadeNodeHandlesPublished")
    computed_work = counters(
        sample,
        "style",
        ["elementStyleRecomputations", "elementInheritedStyleRecomputations"],
    )
    computed_reactions = counter(sample, "style", "elementComputedStyleChanges")
    observer_work = counter(sample, "style", "styleEngineReactionElements")
    observer_reactions = counter(sample, "style", "committedStyleObserverConsequences")
    style_update_microseconds = counter(sample, "style", "styleUpdateMicroseconds")
    accounted_microseconds = counters(
        sample,
        "style",
        [
            "styleEngineTransactionSetupMicroseconds",
            "styleEnginePlanningMicroseconds",
            "styleRecomputeMicroseconds",
        ],
    )
    return {
        "actions": {
            "raw_mutation_records": counter(sample, "graph", "rawMutationRecords"),
            "normalized_actions": normalized_actions,
            "tree": counter(sample, "graph", "treeDeltas"),
            "local_feature": counter(sample, "graph", "localFeatureDeltas"),
            "state": counter(sample, "graph", "stateDeltas"),
            "element_declaration": counter(sample, "graph", "elementDeclarationDeltas"),
            "program": counter(sample, "graph", "programDeltas"),
            "cascade_topology": counter(sample, "graph", "cascadeTopologyDeltas"),
        },
        "selector": {
            "work_units": selector_work,
            "semantic_reactions": selector_reactions,
            "refresh_upqueries": counter(sample, "graph", "selectorTruthRefreshes"),
            "repair_upqueries": counter(sample, "graph", "selectorTruthRepairUpqueries"),
            "repair_additions": counter(sample, "graph", "selectorTruthRepairAdditions"),
            "repair_removals": counter(sample, "graph", "selectorTruthRepairRemovals"),
            "work_per_action_and_reaction": ratio(selector_work, normalized_actions, selector_reactions),
        },
        "match": {
            "work_units": match_work,
            "semantic_reactions": match_reactions,
            "upqueries": counter(sample, "graph", "matchAnswerUpqueries"),
            "work_per_action_and_reaction": ratio(match_work, normalized_actions, match_reactions),
        },
        "cascade": {
            "work_units": cascade_work,
            "published_cascade_node_handle_proxy": cascade_reactions,
            "work_per_action_and_reaction": ratio(cascade_work, normalized_actions, cascade_reactions),
        },
        "computed": {
            "element_evaluations": computed_work,
            "element_output_changes": computed_reactions,
            "parent_inherited_snapshot_builds": counter(sample, "style", "parentInheritedSnapshotBuilds"),
            "parent_inherited_snapshot_properties": counter(sample, "style", "parentInheritedSnapshotProperties"),
            "work_per_action_and_reaction": ratio(computed_work, normalized_actions, computed_reactions),
        },
        "observer": {
            "work_list_elements": observer_work,
            "committed_consequences": observer_reactions,
            "work_per_action_and_reaction": ratio(observer_work, normalized_actions, observer_reactions),
        },
        "published_node_provenance": {
            "direct_action": counter(sample, "graph", "plannedNodesWithDirectAction"),
            "signed_delta": counter(sample, "graph", "plannedNodesWithSignedDelta"),
            "output_change": counter(sample, "graph", "plannedNodesWithOutputChange"),
            "upquery": counter(sample, "graph", "plannedNodesWithUpquery"),
            "unattributed": counter(sample, "graph", "plannedNodesUnattributed"),
        },
        "physical_factorization": {
            "logical_region_nodes": counter(sample, "graph", "exactRegionBatchNodes"),
            "physical_region_intervals": counter(sample, "graph", "exactRegionBatchIntervals"),
            "match_answer_identities": counter(sample, "graph", "matchAnswerSignatures"),
            "match_answer_identity_reuses": counter(sample, "graph", "matchAnswerSignatureReuses"),
            "published_match_answer_records": counter(sample, "graph", "publishedMatchAnswerRecords"),
            "published_match_answer_identity_reads": counter(sample, "graph", "publishedMatchAnswerIdentityReads"),
            "published_match_answer_payload_consumptions": counter(sample, "graph", "publishedMatchAnswerConsumptions"),
            "published_match_answer_shared_payloads": counter(sample, "graph", "publishedMatchAnswerSharedPayloads"),
            "published_match_answer_shared_payload_reuses": counter(
                sample, "graph", "publishedMatchAnswerSharedPayloadReuses"
            ),
            "published_match_answer_contextual_payloads": counter(
                sample, "graph", "publishedMatchAnswerContextualPayloads"
            ),
            "published_match_answer_identity_repairs": counter(sample, "graph", "publishedMatchAnswerIdentityRepairs"),
            "published_match_answer_identity_repair_stops": counter(
                sample, "graph", "publishedMatchAnswerIdentityRepairStops"
            ),
            "cascade_node_handle_publications": cascade_reactions,
            "cascade_states_interned": counter(sample, "graph", "cascadeStatesInterned"),
            "cascade_winner_groups_interned": counter(sample, "graph", "cascadeWinnerGroupsInterned"),
            "cascade_winner_entries_interned": counter(sample, "graph", "cascadeWinnerEntriesInterned"),
            "shared_style_computations": counter(sample, "style", "elementStyleSharedComputations"),
        },
        "typed_gaps": {
            "selector_truth_refreshes": counter(sample, "graph", "selectorTruthRefreshes"),
            "selector_truth_repair_upqueries": counter(sample, "graph", "selectorTruthRepairUpqueries"),
            "match_answer_upqueries": counter(sample, "graph", "matchAnswerUpqueries"),
            "cold_batch_missing_rows": counter(sample, "graph", "coldMatchingBatchMissingRows"),
            "prefix_convergence_upqueries": counter(sample, "graph", "prefixConvergenceUpqueries"),
            "unattributed_published_nodes": counter(sample, "graph", "plannedNodesUnattributed"),
        },
        "timing_microseconds": {
            "wall": sample["wallMicroseconds"],
            "style_update": style_update_microseconds,
            "transaction_setup": counter(sample, "style", "styleEngineTransactionSetupMicroseconds"),
            "planning": counter(sample, "style", "styleEnginePlanningMicroseconds"),
            "style_recompute": counter(sample, "style", "styleRecomputeMicroseconds"),
            "cascade": counter(sample, "style", "styleCascadeMicroseconds"),
            "computed_values": counter(sample, "style", "styleValuesMicroseconds"),
            "parent_inherited_snapshot": counter(sample, "style", "parentInheritedSnapshotMicroseconds"),
            "accounted_operator_total": accounted_microseconds,
            "outside_accounted_operators": max(0, style_update_microseconds - accounted_microseconds),
        },
    }


def add_samples(samples):
    result = {"wallMicroseconds": 0, "graph": {}, "style": {}}
    for sample in samples:
        result["wallMicroseconds"] += sample["wallMicroseconds"]
        for domain in ["graph", "style"]:
            for name, value in sample[domain].items():
                result[domain][name] = result[domain].get(name, 0) + value
    return result


def timing_summary(samples, name):
    values = [counter(sample, "style", name) for sample in samples]
    return {
        "minimum": min(values),
        "median": statistics.median(values),
        "maximum": max(values),
    }


def summarize_suite(profiles, target_flushes, include_raw_profiles):
    setup_samples = [profile["setup"] for profile in profiles]
    flush_samples = [sample for profile in profiles for sample in profile["flushes"]]
    aggregate_setup = add_samples(setup_samples)
    aggregate_flushes = add_samples(flush_samples)
    average_setup_style_time = counter(aggregate_setup, "style", "styleUpdateMicroseconds") / len(setup_samples)
    average_flush_style_time = counter(aggregate_flushes, "style", "styleUpdateMicroseconds") / len(flush_samples)
    result = {
        "setup": stage_model(aggregate_setup),
        "mutation_flushes": stage_model(aggregate_flushes),
        "flush_count": len(flush_samples),
        "style_update_microseconds_per_flush": timing_summary(flush_samples, "styleUpdateMicroseconds"),
        "projected_current_pipeline_iteration_microseconds": average_setup_style_time
        + average_flush_style_time * target_flushes,
    }
    if include_raw_profiles:
        result["raw_profiles"] = profiles
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--webdriver", default=DEFAULT_WEBDRIVER, help="WebDriver executable")
    parser.add_argument("--url", default=DEFAULT_URL, help="StyleBench runner URL")
    parser.add_argument("--iterations", type=int, default=1, help="fresh processes per suite")
    parser.add_argument("--flushes", type=int, default=5, help="measured mutation flushes per process")
    parser.add_argument("--mutations-per-kind", type=int, default=10, help="mutations of each kind per flush")
    parser.add_argument(
        "--kinds",
        choices=("all", "classes", "leaves", "attributes"),
        default="all",
        help="which mutation kinds each measured flush performs",
    )
    parser.add_argument(
        "--target-flushes-per-suite",
        type=int,
        default=STYLE_BENCH_FLUSHES_PER_ITERATION,
        help="flush count used for the current-pipeline iteration projection",
    )
    parser.add_argument(
        "--include-raw-profiles",
        action="store_true",
        help="include every nonzero raw counter delta in the JSON output",
    )
    parser.add_argument(
        "--suite-indexes",
        type=lambda text: [int(part) for part in text.split(",")],
        default=list(range(6)),
        help="comma-separated StyleBench suite indexes to measure (default all six)",
    )
    parser.add_argument("--output", type=Path, help="also write the JSON result to this path")
    args = parser.parse_args()

    if min(args.iterations, args.flushes, args.mutations_per_kind, args.target_flushes_per_suite) < 1:
        parser.error("iteration, flush, mutation, and target-flush counts must be positive")
    webdriver = str(Path(args.webdriver).resolve())
    if not Path(webdriver).is_file():
        parser.error(f"WebDriver executable not found: {webdriver}")

    by_suite = {}
    for suite_index in args.suite_indexes:
        profiles = []
        for iteration in range(args.iterations):
            print(f"Running suite {suite_index + 1}/6, iteration {iteration + 1}/{args.iterations}...", flush=True)
            profiles.append(
                run_profile(
                    webdriver,
                    args.url,
                    suite_index,
                    args.flushes,
                    args.mutations_per_kind,
                    args.kinds,
                )
            )
        suite_name = profiles[0]["suite"]
        by_suite[suite_name] = summarize_suite(
            profiles,
            args.target_flushes_per_suite,
            args.include_raw_profiles,
        )

    result = {
        "webdriver": webdriver,
        "url": args.url,
        "iterations_per_suite": args.iterations,
        "measured_flushes_per_iteration": args.flushes,
        "mutations_per_kind": args.mutations_per_kind,
        "target_flushes_per_suite": args.target_flushes_per_suite,
        "projected_current_pipeline_six_suite_iteration_microseconds": sum(
            suite["projected_current_pipeline_iteration_microseconds"] for suite in by_suite.values()
        ),
        "coverage_gaps": [
            "derived feedback actions and stabilization passes do not exist before Phase 6",
            "cascadeNodeHandlesPublished is a physical publication proxy, not complete semantic winner changes",
            "computed-value counters are per element, not per property dependency evaluation and change",
            "StyleRecord assignment and change identities do not exist before Phase 5",
            "transition, animation, and stabilization reactions do not exist before Phase 6",
            "bulk distinct work is represented by setup totals until the Phase 8 bulk plan exists",
        ],
        "by_suite": by_suite,
    }
    encoded = json.dumps(result, indent=2)
    print(encoded)
    if args.output:
        args.output.write_text(encoded + "\n")


if __name__ == "__main__":
    main()
