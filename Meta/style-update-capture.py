#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
# SPDX-License-Identifier: BSD-2-Clause

"""Capture live style-update phases; compare matched intervals without adding nested clocks.

Uses only Python's standard library and a built headless WebDriver, on Linux
and macOS alike. Examples:
  Meta/style-update-capture.py capture connected.json --runs 3 --scenario connected
  Meta/style-update-capture.py capture paired.json --config A=Build/a/bin/WebDriver \
      --config B=Build/b/bin/WebDriver --runs 3
  Meta/style-update-capture.py compare paired.json paired.json --a A --b B
  Meta/style-update-capture.py capture bench.json --stylebench-url http://localhost:8000/

The caller supplies a served, unmodified StyleBench checkout. Each config/run
uses a fresh browser profile. Setup, mutations, and other flushes are separate.
Process CPU and RSS come from `ps`, the one process listing both platforms share,
so they carry its resolution: RSS is the resident set at the observation rather
than a kernel-tracked peak, and CPU is whole seconds on Linux and hundredths on
macOS, which leaves it readable only across long intervals. CPU is WebContent
user+system time including IPC, script and rendering; it is not style-only CPU.
Anything a platform cannot supply is skipped with a message and recorded as null.
"""

import argparse
import collections
import contextlib
import hashlib
import json
import math
import os
import platform
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "Tests/LibWeb/Text/input/css/style-engine/resources/style-update-fixture.js"
PHASES = {
    "rust": (
        "transactionMicroseconds",
        [
            "commitMicroseconds",
            "routingPlanningMicroseconds",
            "matchingCascadeMicroseconds",
            "computationPublicationMicroseconds",
            "emitMicroseconds",
            "transactionRemainderMicroseconds",
        ],
    ),
    "cpp": (
        "styleUpdateMicroseconds",
        [
            "styleUpdateSubmissionMicroseconds",
            "styleUpdateBridgeMicroseconds",
            "styleUpdateApplyMicroseconds",
            "styleUpdateRemainderMicroseconds",
        ],
    ),
}


WATCHED_PROCESSES = ("js", "test-web", "test262-runner", "WebContent", "WebWorker", "Ladybird", "WebDriver")
PS_FORMAT = "pid=,pgid=,rss=,time=,lstart=,comm="
PS_START_FIELDS = 5
PS_UNAVAILABLE = False


def cpu_microseconds(cell):
    """Parse a `ps` TIME cell, which is [[DD-]HH:]MM:SS[.ff] on Linux and on macOS."""
    days, _, clock = cell.rpartition("-")
    parts = clock.split(":")
    seconds = float(parts[-1])
    for index, unit in ((2, 60), (3, 3600)):
        if len(parts) >= index:
            seconds += int(parts[-index]) * unit
    return (int(days or 0) * 86400 + seconds) * 1_000_000


def process_table():
    """Snapshot every process: pid, process group, resident set, CPU time, start stamp and name.

    `ps -A -o ...` is the listing Linux and macOS both provide. Where it is missing or
    rejects the format, observations are skipped once with a message and every
    process-derived field stays null; a capture never fails over it.
    """
    global PS_UNAVAILABLE
    if PS_UNAVAILABLE:
        return []
    try:
        listing = subprocess.run(["ps", "-A", "-o", PS_FORMAT], capture_output=True, text=True, timeout=30, check=False)
    except (OSError, subprocess.SubprocessError):
        listing = None
    if listing is None or listing.returncode != 0:
        PS_UNAVAILABLE = True
        print(f"Skipping process observations: `ps -A -o {PS_FORMAT}` is unavailable here", flush=True)
        return []
    rows = []
    for line in listing.stdout.splitlines():
        fields = line.split()
        if len(fields) < 5 + PS_START_FIELDS:
            continue
        try:
            rows.append(
                {
                    "pid": int(fields[0]),
                    "group": int(fields[1]),
                    "rssBytes": int(fields[2]) * 1024,
                    "cpuMicroseconds": cpu_microseconds(fields[3]),
                    "startedAt": " ".join(fields[4 : 4 + PS_START_FIELDS]),
                    # macOS prints an absolute path here where Linux prints the bare name.
                    "name": Path(" ".join(fields[4 + PS_START_FIELDS :])).name,
                }
            )
        except ValueError:
            continue
    return rows


def request(base, method, path, body=None):
    data = None if body is None else json.dumps(body).encode()
    query = urllib.request.Request(base + path, data=data, method=method, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(query, timeout=60) as response:
            return json.load(response)["value"]
    except urllib.error.HTTPError as error:
        raise RuntimeError(error.read().decode()) from error


class Browser:
    def __init__(self, executable, log, resource=None):
        self.executable = executable
        self.log = log
        self.session = None
        self.resource = resource

    def __enter__(self):
        self.profile = tempfile.TemporaryDirectory(prefix="style-update-capture-")
        self.output = self.log.open("x")
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        self.base = f"http://127.0.0.1:{port}"
        options = []
        if self.resource:
            url, source = self.resource
            replacement = Path(self.profile.name) / "runner.js"
            replacement.write_text(source)
            mapping = Path(self.profile.name) / "resources.json"
            mapping.write_text(
                json.dumps(
                    {"substitutions": [{"url": url, "file": str(replacement), "content_type": "text/javascript"}]}
                )
            )
            options = ["--resource-map", str(mapping)]
        self.process = subprocess.Popen(
            [
                str(self.executable),
                "--headless",
                "--expose-internals-object",
                "--profiles-directory",
                self.profile.name,
                "--port",
                str(port),
                *options,
            ],
            stdout=self.output,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        print(f"WebDriver pid={self.process.pid}, log={self.log}", flush=True)
        try:
            deadline = time.monotonic() + 10
            while True:
                if self.process.poll() is not None:
                    raise RuntimeError(f"WebDriver exited; inspect {self.log}")
                try:
                    request(self.base, "GET", "/status")
                    break
                except urllib.error.URLError:
                    if time.monotonic() >= deadline:
                        raise RuntimeError("WebDriver startup timed out") from None
                    time.sleep(0.05)
            self.session = request(self.base, "POST", "/session", {"capabilities": {"alwaysMatch": {}}})["sessionId"]
            self.command("timeouts", {"script": 60000, "pageLoad": 60000})
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def command(self, path, body):
        return request(self.base, "POST", f"/session/{self.session}/{path}", body)

    def script(self, script):
        wrapped = (
            "try { return {result: (function() {\n"
            + script
            + "\n}).call(window)}; } catch (error) { return {error: String(error), stack: error.stack}; }"
        )
        value = self.command("execute/sync", {"script": wrapped, "args": []})
        if "error" in value:
            raise RuntimeError(f"{value['error']}\n{value.get('stack', '')}")
        return value.get("result")

    def __exit__(self, *_):
        try:
            if self.session:
                with contextlib.suppress(OSError, RuntimeError):
                    request(self.base, "DELETE", f"/session/{self.session}")
        finally:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(self.process.pid, signal.SIGTERM)
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(self.process.pid, signal.SIGKILL)
                self.process.wait()
            self.output.close()
            self.profile.cleanup()

    def resources(self):
        # The browser runs in its own process group, so only its WebContent processes are read here;
        # unrelated processes never enter its CPU/RSS.
        return sorted(
            (
                {key: row[key] for key in ["pid", "startedAt", "rssBytes", "cpuMicroseconds"]}
                for row in process_table()
                if row["name"] == "WebContent" and row["group"] == self.process.pid
            ),
            key=lambda row: row["pid"],
        )


def resource_delta(before, after):
    same = bool(before) and [(r["pid"], r["startedAt"]) for r in before] == [(r["pid"], r["startedAt"]) for r in after]
    return {
        "cpuMicroseconds": sum(r["cpuMicroseconds"] for r in after) - sum(r["cpuMicroseconds"] for r in before)
        if same
        else None,
        "rssBytes": max(
            (r["rssBytes"] for r in before + after if r["rssBytes"] is not None),
            default=None,
        ),
        "processesBefore": before,
        "processesAfter": after,
    }


def difference(before, after):
    return {
        lane: {
            key: value - before[lane][key]
            for key, value in fields.items()
            if type(value) in (int, float) and type(before[lane].get(key)) in (int, float)
        }
        for lane, fields in after.items()
    }


def normalize(sample):
    delta = difference(sample["before"], sample["after"])
    for lane, (whole, phases) in PHASES.items():
        values = [delta[lane][key] for key in [whole, *phases]]
        if any(value < 0 or int(value) != value for value in values) or sum(values[1:]) != values[0]:
            raise RuntimeError(f"Non-additive {lane} clocks in {sample['name']}: {values}")
    sample["counters"] = delta
    sample["metrics"] = {
        f"{lane}.{key}": delta[lane][key] for lane, (whole, phases) in PHASES.items() for key in [whole, *phases]
    }
    sample["metrics"].update(
        {
            key: sample.get(key)
            for key in ["mutationMicroseconds", "observationMicroseconds", "cpuMicroseconds", "rssBytes"]
        }
    )
    sample["reachedNodes"] = delta["rust"]["reachedStyleNodes"]
    return sample


def fixture_run(browser, scenario, seed):
    browser.command("url", {"url": "about:blank"})
    browser.script("internals.resetStyleFfiCounters(); internals.updateStyle();")
    before = browser.resources()
    setup = browser.script(
        FIXTURE.read_text()
        + f"\nwindow.fixture = createStyleUpdateFixture(20000, {json.dumps(scenario)}, {seed}, true); return fixture.setup;"
    )
    after = browser.resources()
    samples = [normalize({**setup, **resource_delta(before, after)})]
    previous = setup["after"]
    for index in range(12):
        before = browser.resources()
        sample = browser.script(f"return fixture.step({index});")
        after = browser.resources()
        samples.append(
            normalize(
                {"name": f"before-{sample['name']}", "kind": "other", "before": previous, "after": sample["before"]}
            )
        )
        samples.append(normalize({**sample, **resource_delta(before, after)}))
        previous = sample["after"]
    end = browser.script("return fixture.snapshot();")
    samples.append(normalize({"name": "after-mutations", "kind": "other", "before": previous, "after": end}))
    return samples


def stylebench_run(browser, url, timeout):
    parsed = urllib.parse.urlsplit(url)
    query = dict(urllib.parse.parse_qsl(parsed.query))
    query["iterationCount"] = "1"
    url = urllib.parse.urlunsplit(parsed._replace(query=urllib.parse.urlencode(query)))
    before = browser.resources()
    start = time.monotonic()
    browser.command("url", {"url": url})
    browser.script(
        "if (!styleUpdateCapture.started && !startBenchmark()) throw new Error('StyleBench refused to start');"
    )
    while True:
        if browser.script("return styleUpdateCapture.done;"):
            break
        if time.monotonic() - start > timeout:
            raise RuntimeError("StyleBench completion callback timed out")
        time.sleep(0.1)
    after = browser.resources()
    captured = browser.script("return styleUpdateCapture;")
    samples = captured["samples"]
    actual = [sample["name"] for sample in samples if sample["kind"] == "mutation"]
    if not actual or actual != captured["expected"]:
        raise RuntimeError("StyleBench test boundaries do not match exactly one iteration")
    # Suite totals overlap their setup/test/gap rows. Whole run sums only disjoint rows,
    # including the separate runner document; FFI is process-global and omitted here.
    totals = [sample for sample in samples if sample["kind"] != "suite-total"]
    sums = {}
    for sample in totals:
        for lane, fields in difference(sample["before"], sample["after"]).items():
            for key, value in fields.items():
                sums.setdefault(lane, {}).setdefault(key, 0)
                sums[lane][key] += value
    samples.append(
        {
            "name": "whole-run",
            "kind": "run-total",
            "before": {lane: dict.fromkeys(fields, 0) for lane, fields in sums.items()},
            "after": sums,
            "wallMicroseconds": (time.monotonic() - start) * 1_000_000,
            **resource_delta(before, after),
        }
    )
    return [normalize(sample) for sample in samples]


def command_output(*command):
    return subprocess.check_output(command, cwd=ROOT, text=True).strip()


def web_libraries(build):
    """liblagom-web is .so on Linux and .dylib on macOS; hash whatever this tree actually built."""
    directory = build / "lib"
    name = {"linux": "liblagom-web.so", "darwin": "liblagom-web.dylib"}.get(sys.platform)
    if name and (directory / name).is_file():
        return [directory / name]
    return sorted(path for path in directory.glob("liblagom-web.*") if path.is_file()) if directory.is_dir() else []


def cpu_governors():
    """A Linux-only nicety: absent everywhere else, and never a reason to fail a capture."""
    if sys.platform != "linux":
        return None
    with contextlib.suppress(OSError):
        return sorted(
            {path.read_text().strip() for path in Path("/sys/devices/system/cpu").glob("cpu*/cpufreq/scaling_governor")}
        )
    return None


def configuration(executable):
    build = executable.parent.parent
    paths = [
        executable,
        executable.parent / "Ladybird",
        executable.parent / "WebContent",
        build / "libexec/WebContent",
        *web_libraries(build),
    ]
    hashes = {}
    for path in paths:
        if path.is_file():
            with path.open("rb") as stream:
                digest = hashlib.sha256()
                while chunk := stream.read(1024 * 1024):
                    digest.update(chunk)
                hashes[str(path)] = digest.hexdigest()
    cache = build / "CMakeCache.txt"
    information = build / "Libraries/LibWebView/BuildInformation.h"
    return {
        "executable": str(executable),
        "sha256": hashes,
        "buildInformation": information.read_text() if information.exists() else None,
        "cmake": [
            line
            for line in cache.read_text().splitlines()
            if line.startswith(("CMAKE_BUILD_TYPE:", "CMAKE_CXX_COMPILER:", "CMAKE_CXX_FLAGS", "CMAKE_RUST"))
        ]
        if cache.exists()
        else None,
    }


def capture(args):
    if args.output.exists():
        raise RuntimeError(f"Refusing to overwrite {args.output}")
    stray = [f"{row['pid']} {row['name']}" for row in process_table() if row["name"] in WATCHED_PROCESSES]
    if stray:
        raise RuntimeError("Other browser/test processes are running:\n" + "\n".join(stray))
    configs = {}
    for config in args.config or ["release=Build/release/bin/WebDriver"]:
        name, path = config.split("=", 1)
        if not name or name in configs or not all(c.isalnum() or c in "-_" for c in name):
            raise RuntimeError(f"Invalid or duplicate config name: {name}")
        configs[name] = Path(path).resolve()
    if len(configs) > 2:
        raise RuntimeError("Paired captures support at most two configs")
    result = {
        "version": 1,
        "recordedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "head": command_output("git", "rev-parse", "HEAD"),
        "dirty": command_output("git", "status", "--porcelain"),
        "machine": platform.platform(),
        "cpu": platform.processor() or platform.machine() or None,
        "processors": os.cpu_count(),
        "governors": cpu_governors(),
        "affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
        "workers": 1,
        "seed": args.seed,
        "scenario": "stylebench" if args.stylebench_url else args.scenario,
        "stylebenchUrl": args.stylebench_url,
        "elementCount": None if args.stylebench_url else 20000,
        "fixtureSha256": hashlib.sha256(FIXTURE.read_bytes()).hexdigest(),
        "configs": {name: configuration(path) for name, path in configs.items()},
        "runs": [],
        "boundaries": {
            "clocks": "Rust phases partition transaction; C++ phases partition update. Rust is nested inside bridge. Never sum both ledgers.",
            "cpu": "WebContent user+system time from `ps`; includes IPC, JS, layout and rendering between commands, not style-only CPU. Resolution is one second on Linux and hundredths on macOS, so short intervals often read zero.",
            "rss": "Largest WebContent resident set seen at an interval's endpoints; not a kernel-tracked peak, not interval-local, not summed across processes.",
            "unmeasured": [
                "Style-only CPU",
                "peak live style memory",
                "navigation/startup style intervals",
                "StyleBench DOM-call mutation time and per-step CPU/RSS",
            ],
            "stylebench": "Suite/run totals overlap detail rows. First frame snapshot includes load/setup; later gaps and runner document are other. FFI is process-global and omitted for StyleBench.",
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    resource = None
    if args.stylebench_url:
        runner_url = urllib.parse.urljoin(args.stylebench_url, "resources/benchmark-runner.js")
        with urllib.request.urlopen(runner_url, timeout=30) as response:
            source = response.read().decode()
        result["stylebenchRunnerSha256"] = hashlib.sha256(source.encode()).hexdigest()
        resource = (runner_url, source + "\n;\n" + (ROOT / "Meta/StyleUpdateCapture/stylebench.js").read_text())
    for run in range(args.runs):
        order = list(configs) if run % 2 == 0 else list(reversed(configs))
        for name in order:
            log = args.output.with_name(f"{args.output.stem}-{name}-{run + 1}.webdriver.log")
            with Browser(configs[name], log, resource) as browser:
                samples = (
                    stylebench_run(browser, args.stylebench_url, args.timeout)
                    if args.stylebench_url
                    else fixture_run(browser, args.scenario, args.seed)
                )
            result["runs"].append({"config": name, "run": run + 1, "order": len(result["runs"]), "samples": samples})
            print(f"{name} run {run + 1}: {len(samples)} intervals", flush=True)
    with args.output.open("x") as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(f"Saved {args.output}")


def percentile(values, fraction):
    values = sorted(values)
    return values[max(0, math.ceil(len(values) * fraction) - 1)]


def compare(args):
    left, right = (json.loads(path.read_text()) for path in [args.before, args.after])
    for document in [left, right]:
        if document["version"] != 1:
            raise RuntimeError("Unsupported capture version")
    for key in ["scenario", "seed", "elementCount", "fixtureSha256", "stylebenchUrl", "stylebenchRunnerSha256"]:
        if left.get(key) != right.get(key):
            print(f"UNMATCHED capture {key}: {left.get(key)} vs {right.get(key)}")
            return

    def groups(document, selected):
        names = {run["config"] for run in document["runs"]}
        if selected is None:
            if len(names) != 1:
                raise RuntimeError("Select paired capture configs with --a and --b")
            selected = next(iter(names))
        if selected not in names:
            raise RuntimeError(f"Missing config {selected}")
        rows = collections.defaultdict(list)
        for run in document["runs"]:
            if run["config"] == selected:
                for sample in run["samples"]:
                    rows[(sample["kind"], sample["name"])].append({**sample, "run": run["run"]})
                scored = [sample for sample in run["samples"] if sample["kind"] == "mutation"]
                keys = set().union(*(sample["metrics"] for sample in scored))
                rows[("scored-total", "all-mutations")].append(
                    {
                        "run": run["run"],
                        "metrics": {
                            key: (
                                max(sample["metrics"][key] for sample in scored)
                                if key == "rssBytes"
                                else sum(sample["metrics"][key] for sample in scored)
                            )
                            if all(sample["metrics"].get(key) is not None for sample in scored)
                            else None
                            for key in keys
                        },
                    }
                )
        return rows

    a, b = groups(left, args.a), groups(right, args.b)
    print(
        "Values: median / p95 / max; nearest-rank tails across runs, not confidence intervals. Units are microseconds except RSS bytes."
    )
    print("Rust clocks are nested in C++ bridge. Summary totals overlap their detail rows.")
    for key in sorted(a.keys() | b.keys()):
        if key not in a or key not in b:
            print(f"UNMATCHED interval {key}")
            continue
        if args.summary and key[0] not in ("scored-total", "suite-total", "run-total"):
            continue
        print(f"\n{key[0]}: {key[1]} ({len(a[key])}/{len(b[key])} samples)")
        if len(a[key]) != len(b[key]):
            print("  UNMATCHED run counts")
        paired = args.before.resolve() == args.after.resolve() and args.a != args.b
        metrics = set().union(*(sample["metrics"] for sample in [*a[key], *b[key]]))
        for metric in sorted(metrics):
            av = [sample["metrics"].get(metric) for sample in a[key]]
            bv = [sample["metrics"].get(metric) for sample in b[key]]
            if None in av or None in bv:
                print(
                    f"  {metric}: UNMEASURED ({sum(v is not None for v in av)}/{len(av)}, {sum(v is not None for v in bv)}/{len(bv)})"
                )
                continue
            am, bm = statistics.median(av), statistics.median(bv)
            change = f"{(bm / am - 1) * 100:+.2f}%" if am else "not applicable (A median is zero)"
            print(
                f"  {metric}: {am:.2f}/{percentile(av, 0.95):.2f}/{max(av):.2f} -> {bm:.2f}/{percentile(bv, 0.95):.2f}/{max(bv):.2f}; {change}"
            )
            if paired:
                by_run_a = {sample["run"]: sample["metrics"][metric] for sample in a[key]}
                by_run_b = {sample["run"]: sample["metrics"][metric] for sample in b[key]}
                common = by_run_a.keys() & by_run_b.keys()
                if common:
                    print(
                        f"    Paired B-A median: {statistics.median(by_run_b[run] - by_run_a[run] for run in common):+.2f}; {len(common)} pairs"
                    )
                if by_run_a.keys() != by_run_b.keys():
                    print("    UNMATCHED pairs: " + str(sorted(by_run_a.keys() ^ by_run_b.keys())))
        if key[0] != "scored-total":
            counters = {
                f"{lane}.{field}"
                for sample in [*a[key], *b[key]]
                for lane, fields in sample["counters"].items()
                for field in fields
                if not field.endswith("Microseconds")
            }
            changed = []
            for counter in sorted(counters):
                lane, field = counter.split(".", 1)
                av = [s["counters"].get(lane, {}).get(field) for s in a[key]]
                bv = [s["counters"].get(lane, {}).get(field) for s in b[key]]
                if set(av) != set(bv):
                    changed.append(f"{counter}: {av} -> {bv}")
            print("  Counter value sets differ: " + ("\n    " + "\n    ".join(changed) if changed else "none"))
    for label, document in [("A", left), ("B", right)]:
        print(f"{label} unmeasured boundaries: " + "; ".join(document["boundaries"]["unmeasured"]))


def main():
    def interrupted(signum, _frame):
        raise KeyboardInterrupt(f"Interrupted by signal {signum}")

    signal.signal(signal.SIGTERM, interrupted)
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    subcommands = parser.add_subparsers(dest="command", required=True)
    record = subcommands.add_parser("capture")
    record.add_argument("output", type=Path)
    record.add_argument("--config", action="append", help="name=path/to/WebDriver (repeat for paired A/B)")
    record.add_argument("--runs", type=int, default=3)
    record.add_argument("--scenario", choices=["connected", "detached"], default="connected")
    record.add_argument("--seed", type=int, default=1)
    record.add_argument("--stylebench-url")
    record.add_argument("--timeout", type=float, default=300)
    diff = subcommands.add_parser("compare")
    diff.add_argument("before", type=Path)
    diff.add_argument("after", type=Path)
    diff.add_argument("--a", help="config name in before capture")
    diff.add_argument("--b", help="config name in after capture")
    diff.add_argument("--summary", action="store_true", help="show totals only")
    args = parser.parse_args()
    if args.command == "capture":
        if args.runs < 1 or not 0 <= args.seed <= 0xFFFFFFFF or not 0 < args.timeout <= 1800:
            parser.error("runs must be positive, seed must fit u32, timeout must be in (0, 1800]")
        capture(args)
    else:
        compare(args)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, KeyError) as error:
        sys.exit(str(error))
