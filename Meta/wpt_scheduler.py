#!/usr/bin/env python3

import argparse
import glob
import json
import os
import random
import shutil
import signal
import statistics
import subprocess
import sys
import time

from collections import Counter
from collections import defaultdict
from dataclasses import dataclass
from dataclasses import field
from typing import Optional

DEFAULT_TEST_SECONDS = 1.0
DURATIONS_VERSION = 1


def log(message):
    print(f"[wpt-scheduler {time.strftime('%H:%M:%S')}] {message}", flush=True)


def durations_from_raw_logs(paths):
    samples = defaultdict(list)
    for path in paths:
        started = {}
        with open(path, errors="replace") as f:
            for line in f:
                if '"test_start"' not in line and '"test_end"' not in line:
                    continue
                try:
                    event = json.loads(line)
                except ValueError:
                    continue
                action = event.get("action")
                key = (event.get("thread"), event.get("test"))
                if action == "test_start":
                    started[key] = event["time"]
                elif action == "test_end" and key in started:
                    samples[event["test"]].append((event["time"] - started.pop(key)) / 1000)
    return {test: round(statistics.fmean(values), 3) for test, values in samples.items()}


def load_durations(path):
    if not path or not os.path.exists(path):
        return {}
    with open(path) as f:
        data = json.load(f)
    if data.get("version") != DURATIONS_VERSION:
        log(f"ignoring {path}: unknown durations version {data.get('version')!r}")
        return {}
    return data.get("tests", {})


def save_durations(path, tests):
    tmp = f"{path}.tmp"
    with open(tmp, "w") as f:
        json.dump({"version": DURATIONS_VERSION, "tests": tests}, f, separators=(",", ":"), sort_keys=True)
    os.replace(tmp, path)


def estimate_costs(tests, durations):
    by_dir = defaultdict(list)
    for test, seconds in durations.items():
        by_dir[top_dir(test)].append(seconds)
    dir_median = {d: statistics.median(v) for d, v in by_dir.items()}
    global_median = statistics.median(durations.values()) if durations else DEFAULT_TEST_SECONDS

    costs = {}
    unknown = 0
    for test in tests:
        if test in durations:
            costs[test] = durations[test]
        else:
            unknown += 1
            costs[test] = dir_median.get(top_dir(test), global_median)
    return costs, unknown


def top_dir(test):
    return test.strip("/").split("/", 1)[0]


class LoadMonitor:
    def __init__(self):
        self.prev = self._cpu_times()
        self.busy_ema = 0.0

    @staticmethod
    def _cpu_times():
        with open("/proc/stat") as f:
            values = list(map(int, f.readline().split()[1:]))
        return sum(values), values[3] + values[4]

    def sample(self):
        total, idle = self._cpu_times()
        d_total, d_idle = total - self.prev[0], idle - self.prev[1]
        self.prev = (total, idle)
        if d_total > 0:
            busy = 1 - d_idle / d_total
            self.busy_ema = 0.7 * self.busy_ema + 0.3 * busy
        return self.busy_ema

    @staticmethod
    def cpu_pressure():
        try:
            with open("/proc/pressure/cpu") as f:
                return float(f.readline().split()[1].split("=")[1])
        except OSError:
            return 0.0

    @staticmethod
    def mem_available_gib():
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) / 2**20
        return 0.0


@dataclass
class Slot:
    index: int
    netns: str
    rundir: str


@dataclass
class Batch:
    number: int
    slot: Slot
    tests: list
    cost: float
    directory: str
    process: subprocess.Popen
    started_at: float = 0.0
    first_test_at: Optional[float] = None
    ended: set = field(default_factory=set)
    statuses: Counter = field(default_factory=Counter)
    saw_suite_end: bool = False
    raw_offset: int = 0
    shed: bool = False
    terminate_requested_at: Optional[float] = None
    raw_name: str = "raw.jsonl"
    processes: int = 0

    @property
    def raw_log(self):
        return os.path.join(self.directory, self.raw_name)

    def wpt_pids(self):
        needle = f"--include-file={os.path.join(self.directory, 'tests.txt')}".encode()
        uid = os.getuid()
        pids = []
        for entry in os.listdir("/proc"):
            if not entry.isdigit():
                continue
            try:
                if os.stat(f"/proc/{entry}").st_uid != uid:
                    continue
                with open(f"/proc/{entry}/cmdline", "rb") as f:
                    if needle in f.read():
                        pids.append(int(entry))
            except OSError:
                continue
        return pids

    def terminate(self):
        if self.terminate_requested_at is None:
            self.terminate_requested_at = time.time()
        for pid in self.wpt_pids():
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass

    def kill(self):
        # wptrunner's interrupt handling can hang forever, so clean things up manually.
        try:
            netns = os.stat(f"/run/netns/{self.slot.netns}").st_ino
        except OSError:
            return
        uid = os.getuid()
        for entry in os.listdir("/proc"):
            if not entry.isdigit():
                continue
            try:
                if os.stat(f"/proc/{entry}").st_uid == uid and os.stat(f"/proc/{entry}/ns/net").st_ino == netns:
                    os.kill(int(entry), signal.SIGKILL)
            except OSError:
                continue

    def failed_to_authenticate(self):
        # The outer sudo runs with -n, so an expired sudo session fails the batch before wpt even starts.
        if self.process.returncode == 0 or os.path.exists(self.raw_log):
            return False
        try:
            with open(os.path.join(self.directory, "output.log"), "rb") as f:
                output = f.read(4096)
        except OSError:
            return False
        return b"sudo:" in output and (b"password is required" in output or b"terminal is required" in output)

    def poll_log(self):
        try:
            with open(self.raw_log, "rb") as f:
                f.seek(self.raw_offset)
                data = f.read()
        except FileNotFoundError:
            return
        cut = data.rfind(b"\n") + 1
        self.raw_offset += cut
        for line in data[:cut].splitlines():
            if b'"test_end"' in line:
                try:
                    event = json.loads(line)
                except ValueError:
                    continue
                self.ended.add(event["test"])
                self.statuses[event["status"]] += 1
            elif self.first_test_at is None and b'"test_start"' in line:
                self.first_test_at = time.time()
            elif b'"suite_end"' in line:
                self.saw_suite_end = True


class Scheduler:
    def __init__(self, args, wpt_args, tests, costs):
        self.args = args
        self.wpt_args = wpt_args
        self.costs = costs

        # Make sure to avoid pulling CPU-sensitive "long" tests to the front, otherwise pile all the long-running low-load tests at the start.
        def is_deferred(test):
            return any(test.startswith(prefix) for prefix in args.defer)

        self.queue = self.longest_first(t for t in tests if not is_deferred(t))
        self.deferred = [t for t in tests if is_deferred(t)]
        self.cpu_bound = set(self.deferred)
        self.queued_cost = sum(costs.values())
        self.total_tests = len(tests)
        self.retries = Counter()
        self.free_slots = [Slot(i, ns, rundir) for i, (ns, rundir) in enumerate(args.slots)]
        self.running = []
        self.finished = []
        self.next_batch = 0
        self.last_launch = 0.0
        self.load = LoadMonitor()
        self.stopping = False
        self.authentication_failed = False
        self.last_sudo_refresh = time.time()
        self.peak_running = 0
        self.target_instances = min(self.args.min_instances, len(self.free_slots))
        self.memory_scope = self.args.instance_memory_max_gib > 0 and memory_scopes_available()
        if self.args.instance_memory_max_gib > 0 and not self.memory_scope:
            log("systemd user scopes aren't available; instances will run without a memory limit")
        self.last_adjust = 0.0

    def batch_target_seconds(self):
        width = len(self.args.slots) * self.args.processes
        target = self.queued_cost / (2 * width)
        # Near the end the floor would leave instances idle while a few long batches finish, so split whatever is
        # left across the instances we'd run anyway, without going so small that starting them dominates.
        spare_width = max(self.target_instances - len(self.running), 1) * self.args.processes
        if self.queued_cost / spare_width < self.args.min_batch_seconds:
            return max(self.queued_cost / spare_width, self.args.endgame_batch_seconds)
        target = min(max(target, self.args.min_batch_seconds), self.args.max_batch_seconds)
        return target * random.uniform(1 - self.args.batch_jitter, 1 + self.args.batch_jitter)

    def longest_first(self, tests):
        return sorted(tests, key=lambda t: (-self.costs[t], t))

    def pending(self):
        return len(self.queue) + len(self.deferred)

    def is_slow(self, test):
        # Slow here means "mostly waiting for a timeout"; deferred tests are slow because they're busy.
        return self.costs[test] >= self.args.slow_test_seconds and test not in self.cpu_bound

    def release_deferred(self):
        # The opening is over once no slow (timeout-heavy) tests are left to hand out.
        if not self.deferred:
            return
        slow_queued = self.queue and self.is_slow(self.queue[0])
        slow_running = any(b.processes > self.args.processes for b in self.running)
        if self.queue and (slow_queued or slow_running):
            return
        log(f"releasing {len(self.deferred)} deferred tests")
        self.queue = self.longest_first(self.queue + self.deferred)
        self.deferred = []

    def take_batch(self):
        # Slow tests are nearly all waiting out a timeout, which costs next to no CPU, so batches of them get more runners.
        self.release_deferred()
        processes = self.args.processes
        if self.is_slow(self.queue[0]):
            processes *= self.args.slow_test_runner_factor
        target_cost = self.batch_target_seconds() * processes
        tests, cost = [], 0.0
        while self.queue and (cost < target_cost or not tests):
            test = self.queue.pop(0)
            tests.append(test)
            cost += self.costs[test]
        self.queued_cost -= cost
        return tests, cost, processes

    def adjust_target(self, now):
        if now - self.last_adjust < self.args.adjust_interval:
            return
        self.last_adjust = now
        busy = self.load.busy_ema
        pressure = self.load.cpu_pressure()
        if pressure > self.args.max_cpu_pressure:
            self.target_instances = len(self.running) - 1
        elif busy < self.args.target_busy and len(self.running) >= self.target_instances:
            self.target_instances += 2 if busy < self.args.target_busy / 2 else 1
        self.target_instances = max(self.args.min_instances, min(self.target_instances, len(self.args.slots)))

    def shed_memory(self):
        if self.load.mem_available_gib() >= self.args.mem_reserve_gib / 2:
            return
        candidates = [b for b in self.running if not b.shed]
        if len(candidates) <= 1:
            return
        victim = max(candidates, key=lambda b: b.started_at)
        victim.shed = True
        self.target_instances = max(self.args.min_instances, len(self.running) - 1)
        log(f"only {self.load.mem_available_gib():.1f}G available: shedding batch {victim.number}")
        victim.terminate()

    def should_launch(self, now):
        if not self.pending() or not self.free_slots or self.stopping or self.authentication_failed:
            return False
        if len(self.running) < self.args.min_instances:
            return True
        if len(self.running) >= self.target_instances:
            return False
        if now - self.last_launch < self.args.launch_interval:
            return False
        return self.load.mem_available_gib() >= self.args.mem_reserve_gib

    def launch(self, now):
        slot = self.free_slots.pop(0)
        tests, cost, processes = self.take_batch()
        number = self.next_batch
        self.next_batch += 1

        directory = os.path.join(self.args.out, "batches", f"{number:04d}")
        os.makedirs(directory, exist_ok=True)
        include_file = os.path.join(directory, "tests.txt")
        with open(include_file, "w") as f:
            f.write("\n".join(tests) + "\n")

        # We need a raw log of every batch; if one was asked for anyway, that's the one.
        raw_name = raw_log_name(self.args)
        log_args = ["--log-raw", os.path.join(directory, raw_name)]
        for log_type, path in self.args.log:
            if log_type != "--log-raw":
                log_args += [log_type, os.path.join(directory, os.path.basename(path))]

        wpt = [
            os.path.join(self.args.venv, "bin", "python3"),
            "./wpt",
            "--venv",
            self.args.venv,
            "--skip-venv-setup",
            "run",
            "-f",
            "--no-manifest-update",
            "--no-install-fonts",
            f"--processes={processes}",
            f"--include-file={include_file}",
            *log_args,
            *self.wpt_args,
        ]
        path = os.pathsep.join([os.path.join(self.args.venv, "bin"), os.environ["PATH"]])
        environment = [f"PATH={path}", f"VIRTUAL_ENV={self.args.venv}"]
        if self.memory_scope:
            environment += [f"{name}={os.environ[name]}" for name in ("XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS")]
            wpt = [  # Avoid taking the entire instance down because of OOM; let just the one offending process die.
                "systemd-run",
                "--user",
                "--scope",
                "--quiet",
                f"--unit=wpt-{os.getpid()}-batch-{number}",
                "-p",
                f"MemoryMax={self.args.instance_memory_max_gib}G",
                "-p",
                "OOMPolicy=continue",
                "--",
                *wpt,
            ]
        command = [
            "sudo",
            "-n",
            "ip",
            "netns",
            "exec",
            slot.netns,
            "sudo",
            "-u",
            self.args.user,
            "--",
            "env",
            *environment,
            *wpt,
        ]

        # This stays in our session: sudo's cached credentials are usually per terminal.
        with open(os.path.join(directory, "output.log"), "wb") as output:
            process = subprocess.Popen(
                command, cwd=slot.rundir, stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT
            )
        batch = Batch(
            number, slot, tests, cost, directory, process, started_at=now, raw_name=raw_name, processes=processes
        )
        self.running.append(batch)
        self.last_launch = now
        self.peak_running = max(self.peak_running, len(self.running))
        log(
            f"batch {number} -> {slot.netns}: {len(tests)} tests on {processes} runners, ~{cost / processes:.0f}s "
            f"({len(self.running)}/{self.target_instances} running, cpu {self.load.busy_ema * 100:.0f}%, "
            f"psi {self.load.cpu_pressure():.0f}%, {self.pending()} queued)"
        )

    def reap(self):
        for batch in list(self.running):
            batch.poll_log()
            if batch.process.poll() is None:
                continue
            batch.poll_log()
            self.running.remove(batch)
            self.free_slots.append(batch.slot)
            self.finished.append(batch)

            missing = [t for t in batch.tests if t not in batch.ended]
            if not missing or self.stopping:
                continue
            if batch.failed_to_authenticate():
                if not self.authentication_failed:
                    log("sudo needs a password again; not starting any more batches")
                self.authentication_failed = True
                continue
            if batch.shed:
                retry = missing
            else:
                retry = [t for t in missing if self.retries[t] < self.args.retries]
                for t in retry:
                    self.retries[t] += 1
            self.queue[:0] = retry
            self.queued_cost += sum(self.costs[t] for t in retry)
            log(
                f"batch {batch.number} exited with {batch.process.returncode} leaving {len(missing)} tests unrun "
                f"({len(retry)} requeued); see {batch.directory}/output.log"
            )

    def refresh_sudo(self, now):
        # Batches use sudo -n, so keep the credentials from timing out during long stretches without launches.
        if now - self.last_sudo_refresh < self.args.sudo_refresh_interval:
            return
        self.last_sudo_refresh = now
        if subprocess.run(["sudo", "-n", "-v"], stdin=subprocess.DEVNULL, capture_output=True).returncode != 0:
            log("couldn't refresh sudo credentials")

    def stop(self, *_):
        if self.stopping:
            return
        self.stopping = True
        log("stopping: terminating running batches")
        for batch in self.running:
            batch.terminate()

    def run(self):
        start = time.time()
        last_status = 0.0
        while self.pending() or self.running:
            now = time.time()
            self.load.sample()
            self.reap()
            for batch in self.running:
                if batch.terminate_requested_at and now - batch.terminate_requested_at > self.args.terminate_grace:
                    batch.kill()
            if self.stopping or self.authentication_failed:
                if not self.running:
                    break
            else:
                self.refresh_sudo(now)
                self.shed_memory()
                self.adjust_target(now)
                while self.should_launch(now):
                    self.launch(now)
            if now - last_status >= self.args.status_interval:
                last_status = now
                self.print_status(start)
            time.sleep(1)
        return time.time() - start

    def print_status(self, start):
        done = sum(len(b.ended) for b in self.finished + self.running)
        log(
            f"{done}/{self.total_tests} done ({done / max(self.total_tests, 1) * 100:.1f}%), "
            f"{len(self.running)}/{self.target_instances} running, {self.pending()} queued, "
            f"elapsed {(time.time() - start) / 60:.1f}m, "
            f"cpu {self.load.busy_ema * 100:.0f}%, psi {self.load.cpu_pressure():.0f}%, "
            f"mem avail {self.load.mem_available_gib():.0f}G"
        )


def memory_scopes_available():
    if not shutil.which("systemd-run") or not all(
        os.environ.get(name) for name in ("XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS")
    ):
        return False
    probe = subprocess.run(
        ["systemd-run", "--user", "--scope", "--quiet", "-p", "MemoryMax=1G", "--", "true"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return probe.returncode == 0


def raw_log_name(args):
    for log_type, path in args.log:
        if log_type == "--log-raw":
            return os.path.basename(path)
    return "raw.jsonl"


def merge_results(args, batches):
    # Each log ends up as <name>.merged in the output directory. (Chunked runs used to produce <name>.run_<N> per
    # instance; keeping a suffix means whatever collects <name>.* from there keeps working.)
    batch_dirs = [b.directory for b in sorted(batches, key=lambda b: b.number)]
    outputs = {}
    merged_names = set()
    for log_type, path in [("--log-raw", raw_log_name(args))] + list(args.log):
        name = os.path.basename(path)
        target = os.path.join(args.out, f"{name}.merged")
        if target in outputs.values():
            continue
        merged_names.add(name)
        parts = [os.path.join(d, name) for d in batch_dirs if os.path.exists(os.path.join(d, name))]
        if log_type == "--log-wptreport":
            parts = [os.path.join(d, name) for d in batch_dirs]
            merge_wptreports(args, parts, target)
        else:
            with open(target, "wb") as out:
                for part in parts:
                    with open(part, "rb") as f:
                        shutil.copyfileobj(f, out)
        outputs[log_type] = target
    for directory in batch_dirs:
        for name in merged_names:
            try:
                os.remove(os.path.join(directory, name))
            except FileNotFoundError:
                pass
    return outputs


WPTREPORT_FROM_RAW = """
import json, sys
sys.path.insert(0, sys.argv[2])
from wptrunner.formatters.wptreport import WptreportFormatter
formatter = WptreportFormatter()
saw_end = False
with open(sys.argv[1]) as f:
    for line in f:
        try:
            data = json.loads(line)
        except ValueError:
            continue  # the line being written when the batch was killed
        saw_end |= data["action"] == "suite_end"
        output = formatter(data)
if not saw_end:
    output = formatter.suite_end({"action": "suite_end", "time": 0})
sys.stdout.write(output)
"""


def wptreport_from_raw(args, raw_log):
    python = os.path.join(args.venv, "bin", "python3")
    wptrunner = os.path.join(args.wpt_root, "tools", "wptrunner")
    result = subprocess.run([python, "-c", WPTREPORT_FROM_RAW, raw_log, wptrunner], capture_output=True, text=True)
    if result.returncode != 0:
        log(f"couldn't rebuild a report from {raw_log}: {result.stderr.strip().splitlines()[-1:]}")
        return None
    return json.loads(result.stdout)


def merge_wptreports(args, parts, target):
    merged = None
    for part in parts:
        try:
            with open(part) as f:
                report = json.load(f)
        except (OSError, ValueError):
            report = wptreport_from_raw(args, os.path.join(os.path.dirname(part), raw_log_name(args)))
            if report is None:
                continue
        if merged is None:
            merged = report
            continue
        merged["results"].extend(report.get("results", []))
        merged["time_start"] = min(merged.get("time_start", 0), report.get("time_start", 0))
        merged["time_end"] = max(merged.get("time_end", 0), report.get("time_end", 0))
    if merged:
        latest = {}
        for result in merged["results"]:
            latest[(result.get("subsuite", ""), result["test"])] = result
        merged["results"] = list(latest.values())
    with open(target, "w") as f:
        json.dump(merged or {"results": []}, f)


def summarize(args, scheduler, wall_seconds, merged_raw):
    # Later results win, as with the merged report.
    test_status = {}
    subtest_status = {}
    with open(merged_raw, errors="replace") as f:
        for line in f:
            if '"test_status"' in line or '"test_end"' in line:
                try:
                    event = json.loads(line)
                except ValueError:
                    continue
                if event["action"] == "test_end":
                    test_status[event["test"]] = event["status"]
                else:
                    subtest_status[(event["test"], event["subtest"])] = event["status"]
    statuses = Counter(test_status.values())
    subtests = Counter(subtest_status.values())
    ran = sum(statuses.values())
    total_subtests = sum(subtests.values())
    print()
    print(f"Total tests run: {ran} of {scheduler.total_tests} in {wall_seconds / 60:.1f} minutes")
    print(f"Batches: {len(scheduler.finished)} (at most {scheduler.peak_running} running at once)")
    print("Test statuses: " + ", ".join(f"{k} {v}" for k, v in statuses.most_common()))
    if total_subtests:
        print(f"Passing subtests: {subtests['PASS']}/{total_subtests} ({subtests['PASS'] / total_subtests * 100:.2f}%)")
    unrun = scheduler.total_tests - ran
    if unrun > 0:
        print(f"Tests that never reported a result: {unrun}")


def command_run(args, wpt_args):
    with open(args.tests) as f:
        tests = [line.rstrip("\n") for line in f if line.strip()]
    if not tests:
        log("no tests to run")
        return 0

    durations = load_durations(args.durations)
    costs, unknown = estimate_costs(tests, durations)
    log(
        f"{len(tests)} tests, ~{sum(costs.values()) / 60:.0f} slot-minutes estimated "
        f"({unknown} without a recorded duration), {len(args.slots)} slots x {args.processes} runners"
    )

    os.makedirs(args.out, exist_ok=True)
    scheduler = Scheduler(args, wpt_args, tests, costs)
    signal.signal(signal.SIGTERM, scheduler.stop)
    signal.signal(signal.SIGINT, scheduler.stop)
    signal.signal(signal.SIGHUP, scheduler.stop)

    wall_seconds = scheduler.run()

    outputs = merge_results(args, scheduler.finished)
    summarize(args, scheduler, wall_seconds, outputs["--log-raw"])
    print(f"Results in {args.out}")

    if scheduler.authentication_failed:
        log("stopped early because sudo couldn't authenticate")
        return 1

    if not scheduler.stopping and os.path.getsize(outputs["--log-raw"]):
        updated = dict(durations)
        updated.update(durations_from_raw_logs([outputs["--log-raw"]]))
        durations_out = os.path.join(args.out, "durations.json")
        save_durations(durations_out, updated)
        log(f"wrote {durations_out}")

    return 130 if scheduler.stopping else 0


def command_durations(args):
    paths = [p for pattern in args.raw_logs for p in sorted(glob.glob(pattern))]
    if not paths:
        print("no raw logs matched", file=sys.stderr)
        return 1
    tests = load_durations(args.output) if args.merge else {}
    tests.update(durations_from_raw_logs(paths))
    save_durations(args.output, tests)
    print(f"wrote {len(tests)} durations from {len(paths)} logs to {args.output}")
    return 0


def parse_log(value):
    log_type, _, path = value.partition("=")
    if not log_type.startswith("--log-") or not path:
        raise argparse.ArgumentTypeError("expected --log-TYPE=PATH")
    return log_type, path


def parse_slot(value):
    netns, _, rundir = value.partition(":")
    if not netns or not rundir:
        raise argparse.ArgumentTypeError("expected NETNS:RUNDIR")
    return netns, rundir


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    run = commands.add_parser("run", help="run tests; arguments after -- are passed to `wpt run`")
    run.add_argument(
        "--slot",
        dest="slots",
        type=parse_slot,
        action="append",
        required=True,
        help="NETNS:RUNDIR, one per network namespace an instance may run in",
    )
    run.add_argument("--venv", required=True, help="prepared wpt virtualenv, shared by all instances")
    run.add_argument("--wpt-root", default=os.getcwd(), help="the wpt checkout (default: the current directory)")
    run.add_argument("--user", default=os.environ.get("USER"), help="user to run instances as inside the netns")
    run.add_argument("--tests", required=True, help="file listing test ids to run, one per line")
    run.add_argument(
        "--durations",
        help="durations file to order and size batches by, usually the previous run's; an "
        "updated one is written to the output directory",
    )
    run.add_argument("--out", required=True, help="directory for batch logs and merged results")
    run.add_argument(
        "--log",
        type=parse_log,
        action="append",
        default=[],
        metavar="TYPE=PATH",
        help="mozlog output to produce, e.g. --log=--log-wptreport=report.json (merged into --out)",
    )
    run.add_argument("--processes", type=int, default=4, help="test runners per instance")
    run.add_argument(
        "--defer",
        action="append",
        default=None,
        metavar="PREFIX",
        help="hold tests under this path back until the slow, timeout-heavy opening is over "
        "(default: /wasm/, which is CPU-heavy and timing-sensitive); repeatable",
    )
    run.add_argument(
        "--slow-test-seconds",
        type=float,
        default=5.0,
        help="batches starting with tests expected to take at least this long get more runners",
    )
    run.add_argument(
        "--slow-test-runner-factor", type=int, default=2, help="runner multiplier for batches of slow tests"
    )
    run.add_argument(
        "--min-instances",
        type=int,
        default=max((os.cpu_count() or 1) // 8, 1),
        help="instances to keep running regardless of load",
    )
    run.add_argument(
        "--target-busy",
        type=float,
        default=0.85,
        help="run more instances while overall CPU use is below this fraction",
    )
    run.add_argument(
        "--max-cpu-pressure",
        type=float,
        default=30.0,
        help="run fewer instances while CPU pressure (PSI some avg10, %%) is above this",
    )
    run.add_argument(
        "--mem-reserve-gib",
        type=float,
        default=16.0,
        help="don't start more instances while less memory than this is available, and give up the "
        "newest one below half of it",
    )
    run.add_argument("--launch-interval", type=float, default=0.5, help="minimum seconds between launches")
    run.add_argument(
        "--adjust-interval",
        type=float,
        default=3.0,
        help="seconds between changes to the number of instances to keep running",
    )
    # Starting an instance costs ~8 CPU-seconds and 10-20s before its first test, so batches shouldn't be much shorter
    # than this; they shouldn't be much longer either, or the scheduler can't react to load.
    run.add_argument("--min-batch-seconds", type=float, default=120.0)
    run.add_argument("--max-batch-seconds", type=float, default=180.0)
    run.add_argument(
        "--endgame-batch-seconds", type=float, default=45.0, help="smallest batch to split the last of the queue into"
    )
    run.add_argument("--batch-jitter", type=float, default=0.25, help="relative random variation of batch lengths")
    run.add_argument(
        "--instance-memory-max-gib",
        type=float,
        default=10.0,
        help="memory limit for each instance (via a systemd user scope), 0 to disable",
    )
    run.add_argument("--retries", type=int, default=1, help="times to requeue tests left unrun by a dead instance")
    run.add_argument(
        "--terminate-grace",
        type=float,
        default=20.0,
        help="seconds a terminated instance gets to shut down before everything in it is killed",
    )
    run.add_argument("--status-interval", type=float, default=30.0)
    run.add_argument(
        "--sudo-refresh-interval", type=float, default=60.0, help="seconds between refreshing sudo credentials"
    )

    durations = commands.add_parser("durations", help="build a durations file from mozlog raw logs")
    durations.add_argument("output")
    durations.add_argument("raw_logs", nargs="+", help="raw log paths or globs")
    durations.add_argument("--merge", action="store_true", help="update an existing durations file")

    argv = sys.argv[1:]
    wpt_args = []
    if "--" in argv:
        split = argv.index("--")
        argv, wpt_args = argv[:split], argv[split + 1 :]
    args = parser.parse_args(argv)

    if args.command == "run":
        if args.defer is None:
            args.defer = ["/wasm/"]
        return command_run(args, wpt_args)
    return command_durations(args)


if __name__ == "__main__":
    sys.exit(main())
