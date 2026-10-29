#!/usr/bin/env python3

import os
import resource
import signal
import subprocess
import sys
import tempfile
import time

CHILD = r"""
import ctypes, os, signal, sys
size, undumpable, signal_number = int(sys.argv[1]), sys.argv[2] == "1", int(sys.argv[3])
data = bytearray(size)
if size:
    data[::4096] = b"\x01" * len(range(0, size, 4096))
if undumpable:
    ctypes.CDLL(None).prctl(4, 0, 0, 0, 0)
if signal_number != signal.SIGKILL:
    signal.signal(signal_number, signal.SIG_DFL)
sys.stdout.write("ready\n")
sys.stdout.flush()
sys.stdin.readline()
os.kill(os.getpid(), signal_number)
"""


def run_case(label, count, size_mb, core_limit=None, undumpable=False, signal_number=signal.SIGTRAP, cwd=None):
    def preexec():
        if core_limit is not None:
            resource.setrlimit(resource.RLIMIT_CORE, (core_limit, core_limit))

    processes = [
        subprocess.Popen(
            [sys.executable, "-c", CHILD, str(size_mb << 20), "1" if undumpable else "0", str(int(signal_number))],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            preexec_fn=preexec,
            cwd=cwd,
        )
        for _ in range(count)
    ]
    for process in processes:
        assert process.stdin and process.stdout
        assert process.stdout.readline() == b"ready\n"

    started_at = time.monotonic()
    for process in processes:
        assert process.stdin
        process.stdin.write(b"go\n")
        process.stdin.flush()

    exits = []
    for _ in processes:
        pid, status = os.waitpid(-1, 0)
        exited_after = time.monotonic() - started_at
        dumped = os.WIFSIGNALED(status) and os.WCOREDUMP(status)
        exits.append(f"{exited_after:6.2f}s{' core' if dumped else ''}")
    print(f"{label:<44} {', '.join(exits)}", flush=True)


def main():
    print(f"RLIMIT_CORE: {resource.getrlimit(resource.RLIMIT_CORE)}")
    with open("/proc/sys/kernel/core_pattern") as file:
        print(f"core_pattern: {file.read().strip()}")
    print(flush=True)

    with tempfile.TemporaryDirectory() as directory:
        for _ in range(3):
            run_case("SIGKILL, 1024 MB", 1, 1024, signal_number=signal.SIGKILL, cwd=directory)
            run_case("SIGTRAP, 0 MB", 1, 0, cwd=directory)
            run_case("SIGTRAP, 256 MB", 1, 256, cwd=directory)
            run_case("SIGTRAP, 1024 MB", 1, 1024, cwd=directory)
            run_case("SIGTRAP, 1024 MB, RLIMIT_CORE=0", 1, 1024, core_limit=0, cwd=directory)
            run_case("SIGTRAP, 1024 MB, RLIMIT_CORE=1", 1, 1024, core_limit=1, cwd=directory)
            run_case("SIGTRAP, 1024 MB, undumpable", 1, 1024, undumpable=True, cwd=directory)
            run_case("SIGTRAP, 4 at once, 512 MB each", 4, 512, cwd=directory)
            print(flush=True)

        print("Files left in the working directory:", flush=True)
        subprocess.run(["ls", "-la", directory])


if __name__ == "__main__":
    main()
