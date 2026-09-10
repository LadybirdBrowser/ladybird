#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import http.server
import json
import runpy
import subprocess
import sys
import tempfile
import threading

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))
request = helpers["request"]
hits = []
release_downloads = threading.Event()
release_stall = threading.Event()


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        hits.append(self.path)
        if self.path == "/favicon.ico":
            self.send_error(404)
            return
        if self.path == "/page":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(b"<!doctype html><body>")
            return
        if self.path == "/before-stall":
            release_downloads.wait()
        if self.path == "/stall":
            self.send_response(200)
            self.send_header("Content-Length", "5")
            self.end_headers()
            self.wfile.write(b"!")
            self.wfile.flush()
            release_stall.wait()
            return
        status = int(self.path.rsplit("/", 1)[1]) if self.path.startswith("/redirect/") else 302
        self.send_response(200 if self.path in ("/list", "/before-stall", "/after-stall") else status)
        if self.path != "/missing":
            self.send_header(
                "Location",
                "file:///not-a-filter-list"
                if self.path == "/unsafe"
                else "../list"
                if self.path.startswith("/redirect/")
                else "/loop",
            )
        self.send_header("Content-Length", "5")
        self.end_headers()
        self.wfile.write(b"!list")

    def log_message(self, format, *args):
        pass


with tempfile.TemporaryDirectory() as directory:
    profile = Path(directory)
    (profile / "config").mkdir()
    (profile / "legacy.txt").write_text("!legacy")
    (profile / "config" / "Settings.json").write_text(
        json.dumps(
            {
                "contentBlockers": {"builtInLists": {"easyList": False}},
                "configVariables": {"content_blocking.list_paths": [str(profile / "legacy.txt")]},
            }
        )
    )
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    port = helpers["unused_port"]()
    process = subprocess.Popen(
        [sys.argv[1], "--headless", "-l", "127.0.0.1", "-p", str(port), "--profile-path", directory]
    )
    session = None
    try:
        helpers["wait_for_port"](port)
        session = helpers["create_session"](port)

        def command(path, body):
            status, payload, raw = request(port, "POST", f"/session/{session}/{path}", body)
            assert status == 200, raw
            return payload["value"]

        def settings(name="loadCurrentSettings", data=None, wait_for_idle=True):
            return command(
                "execute/async",
                {
                    "script": """
                const [name, data, waitForIdle, done] = arguments;
                const receive = event => {
                    if (event.detail.name !== 'loadSettings') return;
                    if (waitForIdle && event.detail.data.contentBlockerListUpdateInProgress) {
                        setTimeout(() => ladybird.sendMessage('loadCurrentSettings'), 50);
                        return;
                    }
                    document.removeEventListener('WebUIMessage', receive);
                    done(event.detail.data);
                };
                document.addEventListener('WebUIMessage', receive);
                ladybird.sendMessage(name, data);
            """,
                    "args": [name, data, wait_for_idle],
                },
            )

        command("timeouts", {"script": 60000})
        status, payload, raw = request(port, "GET", f"/session/{session}/window")
        assert status == 200, raw
        settings_window = payload["value"]
        page_window = command("window/new", {"type": "tab"})["handle"]
        command("window", {"handle": page_window})
        command("url", {"url": f"http://127.0.0.1:{server.server_port}/page"})
        command(
            "execute/sync",
            {
                "script": """
                window.popup = window.open('about:blank');
                for (const page of [window, popup]) {
                    const advertisement = page.document.createElement('div');
                    advertisement.className = 'advertisement';
                    page.document.body.append(advertisement);
                }
            """,
                "args": [],
            },
        )
        command("window", {"handle": settings_window})
        command("url", {"url": "about:about"})
        command("url", {"url": "about:services"})
        settings()
        command("back", {})
        assert command("execute/sync", {"script": "return location.href", "args": []}) == "about:about"
        command("url", {"url": "about:blocking"})
        state = settings()
        assert command(
            "execute/sync",
            {
                "script": """
                const tab = document.querySelector('[aria-selected="true"]');
                const panel = document.querySelector('#tab-blocking');
                const description = panel.querySelector('.card-body > .description');
                const form = description.nextElementSibling;
                const lists = panel.querySelector('#built-in-content-blocker-lists');
                const heading = panel.querySelector('.filter-list-header .card-title');
                const details = lists.querySelector('.description');
                const metadata = lists.querySelector('.filter-list-metadata');
                return location.hash === '#blocking' && tab.textContent === 'Blocking'
                    && heading.getBoundingClientRect().left === lists.getBoundingClientRect().left
                    && metadata.getBoundingClientRect().top >= details.getBoundingClientRect().bottom
                    && getComputedStyle(metadata).fontSize === '12px'
                    && metadata.textContent.includes('easylist.to') && !details.textContent.includes('easylist.to')
                    && ['label', '.description', 'summary', 'textarea'].every(selector =>
                        getComputedStyle(panel.querySelector(selector)).fontSize === '14px')
                    && form.getBoundingClientRect().top - description.getBoundingClientRect().bottom >= 16;
            """,
                "args": [],
            },
        )
        for enabled in (False, True):
            settings("setServicesNetworkAccessEnabled", enabled)
            assert command(
                "execute/sync",
                {
                    "script": """
                    const toggle = document.querySelector('#filter-list-updates');
                    const style = getComputedStyle(toggle.parentElement);
                    return toggle.disabled === !arguments[0]
                        && style.opacity === (arguments[0] ? '1' : '0.5')
                        && (arguments[0] || style.filter === 'grayscale(1)');
                """,
                    "args": [enabled],
                },
            )
        for rules, display in [("##.advertisement", "none"), ("", "block")]:
            settings("setCustomContentBlockerFilters", rules)
            command("window", {"handle": page_window})
            assert command(
                "execute/sync",
                {
                    "script": "return [window, popup].map(page => page.getComputedStyle(page.document.querySelector('.advertisement')).display)",
                    "args": [],
                },
            ) == [display, display]
            command("window", {"handle": settings_window})
        command("window", {"handle": page_window})
        command("execute/sync", {"script": "popup.close()", "args": []})
        command("window", {"handle": settings_window})
        assert any(item["name"] == "content_blocking.list_paths" for item in state["configVariableDefinitions"])
        assert state["configVariables"]["content_blocking.list_paths"] == [str(profile / "legacy.txt")]
        command(
            "execute/sync",
            {
                "script": """
            const editor = document.querySelector('#custom-content-blocker-filters');
            editor.value = 'unsaved rules';
            editor.dispatchEvent(new Event('input', {bubbles: true}));
            document.querySelector('#tab-button-services').click();
        """,
                "args": [],
            },
        )
        settings("setContentBlockerListEnabled", {"identifier": "easyPrivacy", "enabled": False})
        assert (
            command(
                "execute/sync",
                {"script": "return document.querySelector('#custom-content-blocker-filters').value", "args": []},
            )
            == "unsaved rules"
        )
        command(
            "execute/sync",
            {
                "script": """
                document.querySelector('#save-custom-content-blocker-filters').click();
                const editor = document.querySelector('#custom-content-blocker-filters');
                editor.value = 'edits made while saving';
                editor.dispatchEvent(new Event('input', {bubbles: true}));
            """,
                "args": [],
            },
        )
        assert settings()["contentBlockers"]["customFilters"] == "unsaved rules"

        for path in [f"/redirect/{status}" for status in (301, 302, 303, 307, 308)] + ["/loop", "/unsafe", "/missing"]:
            url = f"http://127.0.0.1:{server.server_port}{path}"
            state = settings("addCustomContentBlockerSubscription", url)
            identifier = next(item["identifier"] for item in state["contentBlockerLists"] if item["url"] == url)
            cached = profile / "data" / "ContentBlocking" / "Lists" / f"{identifier}.txt"
            if path.startswith("/redirect/"):
                assert cached.read_text() == "!list"
            else:
                assert not cached.exists(), path
        assert hits.count("/loop") == 21, hits
        duplicate = settings("addCustomContentBlockerSubscription", f"http://127.0.0.1:{server.server_port}/missing")
        assert len(duplicate["contentBlockerLists"]) == len(state["contentBlockerLists"])
        assert (
            command(
                "execute/sync",
                {
                    "script": "return document.querySelector('#custom-content-blocker-subscription-status').textContent",
                    "args": [],
                },
            )
            == "Subscription already added."
        )
        for path in ("/before-stall", "/stall", "/after-stall"):
            settings("addCustomContentBlockerSubscription", f"http://127.0.0.1:{server.server_port}{path}", False)
        release_downloads.set()
        state = settings()
        for item in state["contentBlockerLists"]:
            if item["url"] and item["url"].endswith(("/before-stall", "/stall", "/after-stall")):
                cached = profile / "data" / "ContentBlocking" / "Lists" / f"{item['identifier']}.txt"
                assert cached.exists() == (not item["url"].endswith("/stall"))

        for character, count in (
            ("a", 64 * 1024 * 1024),
            ("\0", 11 * 1024 * 1024),
            ("\r", 11 * 1024 * 1024),
            ("\f", 11 * 1024 * 1024),
            ("!", 1),
        ):
            status = command(
                "execute/async",
                {
                    "script": """
                const [character, count, done] = arguments;
                const status = document.querySelector('#local-content-blocker-list-status');
                status.textContent = '';
                const observer = new MutationObserver(() => {
                    observer.disconnect();
                    done(status.textContent);
                });
                observer.observe(status, {childList: true, characterData: true, subtree: true});
                const files = new DataTransfer();
                files.items.add(new File([character.repeat(count)], 'import.txt'));
                const picker = document.querySelector('#local-content-blocker-list-picker');
                picker.files = files.files;
                picker.dispatchEvent(new Event('change'));
            """,
                    "args": [character, count],
                },
            )
            assert status == ("import.txt imported." if count == 1 else "The encoded list is too large to import."), (
                status
            )
            settings()
        lists_directory = profile / "data" / "ContentBlocking" / "Lists"
        backup_directory = lists_directory.with_name("Lists-backup")
        lists_directory.rename(backup_directory)
        lists_directory.write_text("Not a directory")
        try:
            settings("importLocalContentBlockerList", {"name": "failed-import.txt", "contents": "!"})
            assert command(
                "execute/sync",
                {
                    "script": "return document.querySelector('#local-content-blocker-list-status').textContent",
                    "args": [],
                },
            ).startswith("Unable to import list:")
        finally:
            lists_directory.unlink()
            backup_directory.rename(lists_directory)
        assert (
            command(
                "execute/sync",
                {"script": "return document.querySelector('#custom-content-blocker-filters').value", "args": []},
            )
            == "edits made while saving"
        )
    finally:
        release_downloads.set()
        release_stall.set()
        if session:
            request(port, "DELETE", f"/session/{session}")
        process.terminate()
        process.wait(timeout=10)
        server.shutdown()
