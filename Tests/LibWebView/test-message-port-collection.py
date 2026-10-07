#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import os
import runpy
import subprocess
import sys
import time

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))

CHANNEL_COUNT = 50
# A thread that has nothing to do with transports can still start between two counts, so a count may come out this
# far above the baseline. A channel the collector missed keeps two threads, one per transport, which the slack can't
# hide.
UNRELATED_THREAD_SLACK = 1

# Transfers one end of each of count channels to the window itself, so both ends are entangled over a transport, which
# runs an IO thread. If started, the received end is given a listener, and then every port is forgotten.
TRANSFER_CHANNELS = """
const [count, started, done] = arguments;
let received = 0;
window.onmessage = event => {
    if (started)
        event.ports[0].onmessage = () => {};
    if (++received === count) {
        window.onmessage = null;
        done();
    }
};
for (let i = 0; i < count; ++i) {
    const channel = new MessageChannel();
    window.postMessage(null, "*", [channel.port2]);
}
"""


def thread_count(pid):
    if sys.platform == "darwin":
        output = subprocess.run(["ps", "-M", "-p", str(pid)], capture_output=True, text=True).stdout
        return len(output.splitlines()) - 1
    return len(os.listdir(f"/proc/{pid}/task"))


def web_content_threads(root_pid):
    output = subprocess.run(["ps", "-axo", "pid=,ppid=,command="], capture_output=True, text=True).stdout
    processes = [line.split(None, 2) for line in output.splitlines()]
    children = {}
    for pid, ppid, command in processes:
        children.setdefault(int(ppid), []).append((int(pid), command))
    total = 0
    pending = [root_pid]
    while pending:
        for pid, command in children.get(pending.pop(), []):
            pending.append(pid)
            if command.split()[0].endswith("/WebContent"):
                total += thread_count(pid)
    return total


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    webdriver_binary = parser.parse_args().webdriver_binary

    port = helpers["unused_port"]()
    process = subprocess.Popen(
        [webdriver_binary, "--headless", "--expose-internals-object", "-l", "127.0.0.1", "-p", str(port)]
    )
    session = None
    try:
        helpers["wait_for_port"](port)
        session = helpers["create_session"](port)

        def run(script, *args, asynchronous=False):
            path = f"/session/{session}/execute/{'async' if asynchronous else 'sync'}"
            status, payload, raw = helpers["request"](port, "POST", path, {"script": script, "args": list(args)})
            assert status == 200, raw
            return payload["value"]

        def collect_and_count(limit):
            run("internals.gc()")
            # The IO thread of a transport exits once collecting its port has closed it.
            deadline = time.monotonic() + helpers["EVENT_TIMEOUT_SECONDS"]
            threads = web_content_threads(process.pid)
            while threads > limit and time.monotonic() < deadline:
                time.sleep(0.1)
                run("internals.gc()")
                threads = web_content_threads(process.pid)
            return threads

        # Running a script makes sure the page's process is up before its threads are counted.
        run("return 1")
        # Threads that have nothing to do with transports start lazily, the allocator's decommit worker among them, so a
        # first round of channels brings them up before the baseline is taken.
        warm_up = web_content_threads(process.pid)
        assert warm_up > 1, f"Found {warm_up} WebContent threads"
        run(TRANSFER_CHANNELS, CHANNEL_COUNT, False, asynchronous=True)
        baseline = collect_and_count(warm_up + UNRELATED_THREAD_SLACK)
        assert baseline <= warm_up + UNRELATED_THREAD_SLACK, (
            f"{baseline} WebContent threads after collecting the warm-up channels, {warm_up} before"
        )

        # Neither end of these channels is started, so no message could ever reach script through them.
        run(TRANSFER_CHANNELS, CHANNEL_COUNT, False, asynchronous=True)
        threads = collect_and_count(baseline + UNRELATED_THREAD_SLACK)
        assert threads <= baseline + UNRELATED_THREAD_SLACK, (
            f"{threads} WebContent threads after collecting {CHANNEL_COUNT} dropped channels, {baseline} before"
        )

        # A started port that script forgot keeps receiving messages from the end it's entangled with.
        received = run(
            """
            const done = arguments[0];
            const channel = new MessageChannel();
            window.onmessage = event => {
                event.ports[0].onmessage = message => done(message.data);
                window.onmessage = null;
                setTimeout(() => {
                    internals.gc();
                    channel.port1.postMessage("still entangled");
                }, 0);
            };
            window.postMessage(null, "*", [channel.port2]);
            """,
            asynchronous=True,
        )
        assert received == "still entangled", received

        # A started port that script transfers again is no longer entangled, so nothing keeps it alive either.
        collected = run(
            """
            const done = arguments[0];
            const channel = new MessageChannel();
            let weakPort;
            window.onmessage = event => {
                const port = event.ports[0];
                if (!weakPort) {
                    port.start();
                    port.expando = "keeps the wrapper";
                    weakPort = new WeakRef(port);
                    window.postMessage(null, "*", [port]);
                    return;
                }
                window.onmessage = null;
                setTimeout(() => {
                    internals.gc();
                    done(weakPort.deref() === undefined);
                }, 0);
            };
            window.postMessage(null, "*", [channel.port2]);
            """,
            asynchronous=True,
        )
        assert collected is True, "A started port that was transferred again was not collected"
        print(f"PASS: {CHANNEL_COUNT} dropped channels collected ({baseline} -> {threads} threads)")
    finally:
        try:
            if session is not None:
                helpers["request"](port, "DELETE", f"/session/{session}")
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    main()
