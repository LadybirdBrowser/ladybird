#!/usr/bin/env python3
"""Navigate to a URL N times through WebDriver and report how long the load took.

Usage: measure-style-load.py <webdriver-port> <url> [runs]

The minimum is the number to compare between two builds; the median says how noisy the machine was.
Start the driver with:

    ./bin/Ladybird.app/Contents/MacOS/WebDriver --headless -p 4540
"""

import json
import statistics
import sys
import urllib.request

PORT = sys.argv[1]
URL = sys.argv[2]
RUNS = int(sys.argv[3]) if len(sys.argv) > 3 else 12
BASE = f"http://127.0.0.1:{PORT}"


def post(path, body):
    req = urllib.request.Request(
        BASE + path, data=json.dumps(body).encode(), headers={"Content-Type": "application/json"}, method="POST"
    )
    with urllib.request.urlopen(req, timeout=300) as r:
        return json.load(r)


session = post("/session", {"capabilities": {"alwaysMatch": {}}})["value"]["sessionId"]
post(f"/session/{session}/timeouts", {"script": 300000, "pageLoad": 300000})

SCRIPT = (
    "const t = performance.timing;"
    "return JSON.stringify({load: t.domComplete - t.navigationStart,"
    " dcl: t.domContentLoadedEventEnd - t.navigationStart});"
)

loads, dcls = [], []
for _ in range(RUNS):
    post(f"/session/{session}/url", {"url": "about:blank"})
    post(f"/session/{session}/url", {"url": URL})
    v = json.loads(post(f"/session/{session}/execute/sync", {"script": SCRIPT, "args": []})["value"])
    loads.append(v["load"])
    dcls.append(v["dcl"])

print(
    json.dumps(
        {
            "runs": RUNS,
            "load_min": min(loads),
            "load_median": statistics.median(loads),
            "dcl_min": min(dcls),
            "dcl_median": statistics.median(dcls),
            "loads": sorted(loads),
        }
    )
)
