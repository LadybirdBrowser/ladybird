#!/usr/bin/env python3
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import concurrent.futures
import importlib.util
import os
import random
import shutil
import signal
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path


SOURCE_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_WORK_DIRECTORY = SOURCE_ROOT / "Build" / "domato"
DEFAULT_LADYBIRD = SOURCE_ROOT / "Build" / "sanitizer" / "bin" / "Ladybird"
CRASH_MARKERS = (
    "ERROR: AddressSanitizer",
    "AddressSanitizer:DEADLYSIGNAL",
    "ERROR: LeakSanitizer",
    "WARNING: MemorySanitizer",
    "UndefinedBehaviorSanitizer",
    "runtime error:",
    "VERIFICATION FAILED",
)


def parse_arguments():
    parser = argparse.ArgumentParser(description="Run generated Domato pages in isolated Ladybird processes")
    parser.add_argument("domato", type=Path, help="Checkout of googleprojectzero/domato")
    parser.add_argument("--ladybird", type=Path, default=DEFAULT_LADYBIRD, help="Ladybird executable")
    parser.add_argument("--work-directory", type=Path, default=DEFAULT_WORK_DIRECTORY)
    parser.add_argument(
        "--count", type=int, default=0, help="Total samples to run (default: keep running until interrupted)"
    )
    parser.add_argument("--batch-size", type=int, default=64, help="Samples to generate in each batch")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 8), help="Concurrent Ladybird processes")
    parser.add_argument("--run-seconds", type=float, default=5, help="Time to exercise each page before restarting")
    parser.add_argument("--main-lines", type=int, default=100, help="Domato statements in the main function")
    parser.add_argument("--event-handler-lines", type=int, default=20, help="Domato statements per event handler")
    parser.add_argument("--seed", type=int, help="Seed for the first batch (incremented for later batches)")
    return parser.parse_args()


def validate_arguments(args):
    if not (args.domato / "generator.py").is_file():
        raise ValueError(f"Domato generator not found: {args.domato / 'generator.py'}")
    if not args.ladybird.is_file():
        raise ValueError(f"Ladybird executable not found: {args.ladybird}")
    for name in ("count", "batch_size", "jobs", "run_seconds", "main_lines", "event_handler_lines"):
        if getattr(args, name) < 0 or (name != "count" and getattr(args, name) == 0):
            raise ValueError(f"--{name.replace('_', '-')} must be positive")


def load_domato(domato_directory):
    sys.path.insert(0, str(domato_directory))
    spec = importlib.util.spec_from_file_location("ladybird_domato_generator", domato_directory / "generator.py")
    if spec is None or spec.loader is None:
        raise RuntimeError("Could not import Domato's generator")

    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    return generator


def create_batch_root(work_directory):
    work_directory.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix="batch-", dir=work_directory))


def generate_batch(generator, args, batch_root, batch_size, batch_number):
    generator._N_MAIN_LINES = args.main_lines
    generator._N_EVENTHANDLER_LINES = args.event_handler_lines
    if args.seed is not None:
        random.seed(args.seed + batch_number)

    output_directory = batch_root / "inputs"
    output_directory.mkdir()
    output_files = [str(output_directory / f"fuzz-{index:05}.html") for index in range(batch_size)]
    template = (Path(__file__).parent / "template.html").read_text()
    generator.generate_samples(template, output_files)
    return [Path(path) for path in output_files]


def terminate_process_group(process):
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=1)
    except subprocess.TimeoutExpired:
        pass

    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    if process.poll() is None:
        process.wait()


def run_sample(ladybird, sample, log, run_seconds, environment):
    exited_early = False
    return_code = None
    sample_environment = environment.copy()
    sample_state = log.parent / sample.stem
    sample_state.mkdir()
    sample_environment["XDG_DATA_HOME"] = str(sample_state)
    with log.open("wb") as output:
        try:
            process = subprocess.Popen(
                [str(ladybird), "--headless=manual", sample.resolve().as_uri()],
                cwd=SOURCE_ROOT,
                env=sample_environment,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=output,
                start_new_session=True,
            )
        except OSError as error:
            output.write(f"Could not start Ladybird: {error}\n".encode())
            return sample, "could not start Ladybird"

        try:
            return_code = process.wait(timeout=run_seconds)
            exited_early = True
        except subprocess.TimeoutExpired:
            pass
        finally:
            terminate_process_group(process)

    diagnostics = log.read_text(errors="replace")
    marker = next((marker for marker in CRASH_MARKERS if marker in diagnostics), None)
    if marker is not None:
        return sample, marker
    if any((sample_state / "Ladybird" / "CrashReports").glob("*.txt")):
        return sample, "Ladybird helper-process crash"
    if exited_early and return_code != 0:
        return sample, f"Ladybird exited with status {return_code}"
    return sample, None


def run_batch(args, samples, batch_root):
    environment = os.environ.copy()
    environment.setdefault(
        "ASAN_OPTIONS",
        "strict_string_checks=1:check_initialization_order=1:strict_init_order=1:"
        "detect_stack_use_after_return=1:allocator_may_return_null=1:detect_leaks=0",
    )
    environment.setdefault("UBSAN_OPTIONS", "print_stacktrace=1:print_summary=1:halt_on_error=1")

    logs = batch_root / "logs"
    logs.mkdir()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {
            executor.submit(
                run_sample,
                args.ladybird.resolve(),
                sample,
                logs / f"{sample.stem}.log",
                args.run_seconds,
                environment,
            ): sample
            for sample in samples
        }
        return [future.result() for future in concurrent.futures.as_completed(futures)]


def preserve_batch(work_directory, batch_root, label, batch_number):
    artifact_directory = work_directory / "artifacts"
    artifact_directory.mkdir(parents=True, exist_ok=True)
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    destination = artifact_directory / f"{label}-{timestamp}-{batch_number:05}"
    shutil.move(batch_root, destination)
    return destination


def main():
    args = parse_arguments()
    try:
        validate_arguments(args)
        generator = load_domato(args.domato.resolve())
    except (OSError, ValueError, RuntimeError) as error:
        print(error, file=sys.stderr)
        return 2

    completed = 0
    batch_number = 0
    while args.count == 0 or completed < args.count:
        batch_size = args.batch_size if args.count == 0 else min(args.batch_size, args.count - completed)
        batch_root = create_batch_root(args.work_directory)
        samples = generate_batch(generator, args, batch_root, batch_size, batch_number)
        print(f"Running Domato batch {batch_number + 1} ({batch_size} samples, {completed} completed)", flush=True)

        try:
            results = run_batch(args, samples, batch_root)
        except KeyboardInterrupt:
            destination = preserve_batch(args.work_directory, batch_root, "interrupted", batch_number)
            print(f"Stopped; retained the active batch in {destination}")
            return 130

        failures = [(sample, reason) for sample, reason in results if reason is not None]
        if failures:
            destination = preserve_batch(args.work_directory, batch_root, "failure", batch_number)
            for sample, reason in failures:
                print(f"{reason}: {destination / 'inputs' / sample.name}")
            print(f"Retained inputs and diagnostics in {destination}")
            return 1

        shutil.rmtree(batch_root)
        completed += batch_size
        batch_number += 1

    print(f"Completed {completed} Domato samples without a sanitizer finding")
    return 0


if __name__ == "__main__":
    sys.exit(main())
