#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import http.server
import runpy
import tempfile
import threading

from pathlib import Path

devtools = runpy.run_path(str(Path(__file__).with_name("test-devtools-client-follows-the-tab.py")))
PAGE = b"""<!doctype html><title>console-previews</title><dfn id="definition">Definition</dfn>
<script>
window.getterCalls = 0;
const inherited = { get inherited() { ++getterCalls; return 1; } };
const object = Object.create(inherited, {
    own: { enumerable: true, get() { ++getterCalls; return 2; } },
});
console.log("before watching", object);
window.loaded = true;
</script>"""
WORKER = b"""
let calls = 0;
const object = Object.create({ get inherited() { ++calls; return 1; } }, {
    own: { enumerable: true, get() { ++calls; return 2; } },
});
console.log(object);
console.log(new Array(0xffffffff));
postMessage(calls);
"""


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/javascript" if self.path == "/worker.js" else "text/html")
        self.end_headers()
        self.wfile.write(WORKER if self.path == "/worker.js" else PAGE)

    def log_message(self, format, *args):
        pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("ladybird_binary")
    binary = parser.parse_args().ladybird_binary
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{server.server_port}/page"
    try:
        with tempfile.TemporaryDirectory(prefix="ladybird-console-previews-") as profile:
            process, _, client = devtools["launch"](binary, profile, url)
            try:
                devtools["wait_for_page"](client, url, "console-previews")
                watcher = client.watch_frame_targets()
                devtools["wait_until"]("the page has logged", lambda: client.evaluate("window.loaded === true"))
                calls = client.evaluate("window.getterCalls")
                assert calls == 0, f"Logging invoked getters {calls} times"
                client.evaluate("""(() => {
                    const worker = new Worker('/worker.js');
                    worker.onmessage = event => { window.workerCalls = event.data; worker.terminate(); };
                    worker.onerror = () => { window.workerCalls = 'error'; };
                })()""")
                devtools["wait_until"]("the worker has logged", lambda: client.evaluate("'workerCalls' in window"))
                assert client.evaluate("window.workerCalls") == 0, "Worker logging invoked getters or failed"

                messages = []
                receive = client.receive

                def receive_and_record():
                    packet = receive()
                    if packet.get("type") == "resources-available-array":
                        for resource_type, resources in packet.get("array", []):
                            if resource_type == "console-message":
                                messages.extend(resources)
                    return packet

                client.receive = receive_and_record

                def logged(label):
                    return next((m for m in messages if m["arguments"][0] == label), None)

                def message_arguments(label):
                    message = logged(label)
                    assert message is not None, label
                    return message["arguments"]

                # These messages must be bounded before a client subscribes, as well as when it is watching.
                client.evaluate("""(() => {
                    const cycle = {}; cycle.self = cycle;
                    console.log('cycle', cycle);
                    console.log('primitive values', undefined, null, true, false, 42, -0, NaN, Infinity, -Infinity, 'text', 42n);
                    console.log('buffered contents', { marker: 'terminal-property-value' });
                    console.log('table begins');
                    console.table([{ marker: 'table-property-value' }]);
                    console.log('large string', 'x'.repeat(1000000));
                    console.log('large array', Array(100000).fill('x'.repeat(1000)));
                    console.log('sparse array', new Array(0xffffffff));
                    console.log('large map', new Map(Array.from({ length: 10000 }, (_, i) => [i, 'x'.repeat(1000)])));
                    console.log('large set', new Set(Array.from({ length: 10000 }, (_, i) => i)));
                    const array = [];
                    Object.defineProperty(array, 0, { get() { ++getterCalls; return 1; } });
                    console.log('array getter', array);
                    let deep = {};
                    for (let i = 0; i < 10000; ++i) deep = { next: deep };
                    console.log('deep object', deep);
                    console.log('large symbol', Symbol('x'.repeat(1000000)));
                    console.log('large bigint', 1n << 1000000n);
                    console.log('many arguments', ...Array(200).fill('x'.repeat(1000)));
                    console.log('many small arguments', ...Array(200).fill('x'));
                    console.log('buffered DOM', document.getElementById('definition'));
                    window.traps = 0;
                    const handler = {};
                    for (const name of ['get', 'getPrototypeOf', 'ownKeys', 'getOwnPropertyDescriptor'])
                        handler[name] = () => { ++traps; throw Error(name); };
                    console.log('proxy', new Proxy({}, handler));
                    return traps;
                })()""")
                assert client.evaluate("window.traps") == 0, "Logging invoked a proxy trap"
                client.send({"to": watcher, "type": "watchResources", "resourceTypes": ["console-message"]})
                client.receive_matching(lambda packet: logged("proxy") is not None)
                for label in [
                    "cycle",
                    "large string",
                    "large array",
                    "sparse array",
                    "large map",
                    "large set",
                    "deep object",
                    "large symbol",
                    "buffered DOM",
                ]:
                    message = logged(label)
                    assert message is not None, label
                    preview = message["arguments"][1]
                    assert isinstance(preview, str) and len(preview) < 16384, (label, type(preview))
                    assert preview, label

                for label, summary in {
                    "before watching": "Object",
                    "cycle": "Object",
                    "buffered contents": "Object",
                    "large array": "Array",
                    "sparse array": "Array",
                    "array getter": "Array",
                    "large map": "Map",
                    "large set": "Set",
                    "deep object": "Object",
                    "proxy": "ProxyObject",
                }.items():
                    assert message_arguments(label)[1] == summary, (label, message_arguments(label))
                assert message_arguments("primitive values")[1:] == [
                    {"type": "undefined"},
                    {"type": "null"},
                    True,
                    False,
                    42,
                    {"type": "-0"},
                    {"type": "NaN"},
                    {"type": "Infinity"},
                    {"type": "-Infinity"},
                    "text",
                    {"type": "BigInt", "text": "42"},
                ]
                table_start = next(i for i, message in enumerate(messages) if message["arguments"] == ["table begins"])
                assert messages[table_start + 1]["arguments"] == ["Object"], messages[table_start + 1]
                assert message_arguments("buffered DOM")[1] == '<dfn id="definition">'
                assert len(message_arguments("large bigint")[1]["text"]) < 16384
                assert sum(len(value) for value in message_arguments("many arguments")) < 16384
                assert message_arguments("many arguments")[-1] == "..."
                assert message_arguments("many small arguments")[-1] == "..."

                client.evaluate("console.log('after watching', object); undefined")
                assert client.evaluate("window.getterCalls") == 0, "A live preview invoked a getter"
                client.evaluate("""(() => {
                    const element = document.getElementById('definition');
                    for (let i = 0; i < 20; ++i) element.setAttribute('data-' + i, 'x'.repeat(100000));
                    Object.defineProperty(element, 'innerText', { get() { ++getterCalls; return 'unexpected'; } });
                    console.log('DOM element', element);
                    console.log('live string', 'x'.repeat(1000000));
                })()""")
                if logged("DOM element") is None:
                    client.receive_matching(lambda packet: logged("DOM element") is not None)
                grip = message_arguments("DOM element")[1]
                assert grip["preview"]["kind"] == "DOMNode", grip
                assert grip["preview"]["nodeName"] == "dfn", grip
                assert len(grip["preview"]["attributes"]) <= 10, grip
                assert all(len(value) < 2048 for value in grip["preview"]["attributes"].values()), grip
                if logged("live string") is None:
                    client.receive_matching(lambda packet: logged("live string") is not None)
                assert len(message_arguments("live string")[1]) < 16384
                assert client.evaluate("window.getterCalls") == 0, "A DOM preview invoked a getter"
                actor = grip["actor"]
                client.send({"to": actor, "type": "prototypeAndProperties"})
                properties = client.receive_matching(lambda packet: packet.get("from") == actor)
                assert "ownProperties" in properties, properties
                print("PASS: Console previews are bounded, avoid getters and proxy traps, and DOM objects expand")
            finally:
                client.close()
                devtools["stop"](process)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
