<!-- Copyright (c) 2026-present, the Ladybird developers. -->
<!-- SPDX-License-Identifier: BSD-2-Clause -->

# Investigating websites with WebDriver

Ladybird's WebDriver service can launch a browser, navigate pages, evaluate
JavaScript, and capture screenshots. It also accepts `--resource-map` to
substitute local files for HTTP or HTTPS resources while retaining their URLs.

## Run a complete example

From the repository root, build WebDriver and its browser dependencies:

```sh
cd Build/release
ninja WebDriver
```

Run the following example from that build directory. It uses Python's standard
library, starts WebDriver on loopback, creates a headless session, and loads a
page and script supplied entirely by a resource map. It prints the script's
result and saves `webdriver-example.png` and `webdriver-example.log` in the
build directory. These two files are overwritten on subsequent runs.

```sh
python3 - <<'PY'
import base64
import http.client
import json
import socket
import subprocess
import tempfile
import time
from pathlib import Path

build = Path.cwd()
webdriver = build / "bin/WebDriver"
if not webdriver.is_file():
    webdriver = build / "bin/Ladybird.app/Contents/MacOS/WebDriver"

with socket.socket() as listener:
    listener.bind(("127.0.0.1", 0))
    port = listener.getsockname()[1]

def request(method, path, body=None):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=60)
    try:
        connection.request(
            method, path,
            body=json.dumps(body) if body is not None else None,
            headers={"Content-Type": "application/json"},
        )
        response = connection.getresponse()
        payload = json.loads(response.read())
        if response.status != 200:
            raise RuntimeError(f"{method} {path}: {response.status}: {payload}")
        return payload["value"]
    finally:
        connection.close()

with tempfile.TemporaryDirectory(prefix="ladybird-webdriver-example-") as temporary:
    root = Path(temporary)
    page = root / "index.html"
    script = root / "probe.js"
    page.write_text(
        '<!doctype html><title>WebDriver example</title>'
        '<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns=%22http://www.w3.org/2000/svg%22/%3E">'
        '<h1 id="result">Waiting for script</h1><script src="/probe.js"></script>',
        encoding="utf-8",
    )
    script.write_text(
        'document.getElementById("result").textContent = "Local override loaded";',
        encoding="utf-8",
    )
    resource_map = root / "resources.json"
    resource_map.write_text(json.dumps({"substitutions": [
        {"url": "https://ladybird.example/", "file": str(page), "content_type": "text/html"},
        {"url": "https://ladybird.example/probe.js", "file": str(script), "content_type": "text/javascript"},
    ]}), encoding="utf-8")

    with (build / "webdriver-example.log").open("w") as log:
        process = subprocess.Popen([
            str(webdriver), "--headless", "--listen-address", "127.0.0.1",
            "--port", str(port), "--resource-map", str(resource_map),
        ], stdout=log, stderr=subprocess.STDOUT)
        session = None
        try:
            deadline = time.monotonic() + 30
            while True:
                if process.poll() is not None:
                    raise RuntimeError("WebDriver exited; inspect webdriver-example.log")
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                        break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise RuntimeError("WebDriver did not start; inspect webdriver-example.log") from None
                    time.sleep(0.05)

            session = request("POST", "/session", {
                "capabilities": {"alwaysMatch": {"ladybird:headless": True}}
            })["sessionId"]
            endpoint = f"/session/{session}"
            request("POST", endpoint + "/timeouts", {"pageLoad": 30000, "script": 30000})
            request("POST", endpoint + "/url", {"url": "https://ladybird.example/"})
            result = request("POST", endpoint + "/execute/sync", {
                "script": "return {url: location.href, text: document.getElementById('result').textContent};",
                "args": [],
            })
            print(json.dumps(result, indent=2))
            assert result["text"] == "Local override loaded", result
            screenshot = request("GET", endpoint + "/screenshot")
            output = build / "webdriver-example.png"
            output.write_bytes(base64.b64decode(screenshot, validate=True))
            print(f"Screenshot: {output}")
        finally:
            try:
                if session is not None:
                    request("DELETE", f"/session/{session}")
            finally:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
PY
```

Deleting the session closes its browser. Terminating WebDriver cleanly removes
the temporary browser profile it creates by default. The Python context manager
removes the example's resource files; the screenshot and log remain available.
WebDriver launches the browser and its helper processes, so they do not need to
be started separately.

## Adapt the example to a website

Change the navigation URL and the JavaScript passed to `/execute/sync`. Remove
`--resource-map` and its argument to load all resources from the network, or
retain a map containing only the resources you want to instrument. For example:

```json
{
  "substitutions": [
    {
      "url": "https://example.com/assets/app.js",
      "file": "/absolute/path/to/instrumented-app.js",
      "content_type": "text/javascript"
    }
  ]
}
```

Use absolute local paths. Entries support an optional `status_code` (default
200), and `content_type` defaults to a guess from the local filename. URL matching
ignores the query and fragment, so the entry above also replaces
`https://example.com/assets/app.js?v=123`. The map is loaded when RequestServer
starts; create the mapped files before starting the session. Restart the
WebDriver session after changing map entries or replacing a mapped file so
the sandbox can grant access to the new file. Mapped files are read when served,
but browser caching can still affect subsequent loads.

Navigation waits for page loading according to the session's page-load strategy.
For application work that continues afterward, use `/execute/async` with an
application event or promise and call the callback supplied as the last script
argument. Avoid fixed sleeps to decide when a page is ready. The example's short
startup polling interval only waits for the WebDriver listening socket.

To inspect a visible browser, remove `--headless` and set `ladybird:headless` to
`false`. For a browser process waiting for a debugger, add, for example,
`--debug-process WebContent` and allow enough request time to attach. Use
`--force-cpu-painting` when investigating differences involving GPU painting.

For authenticated investigations, `--profile-path /absolute/path/to/profile`
selects a persistent profile. WebDriver otherwise uses its own temporary profile.
See `./bin/WebDriver --help` (or the bundle path above on macOS) for other options.
