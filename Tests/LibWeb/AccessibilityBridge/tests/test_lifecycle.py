"""Lifecycle invariants — focus events, tree updates."""

from __future__ import annotations

import http.server
import pathlib
import shutil
import subprocess
import sys
import threading
import time
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402
from harness import FIXTURE_DIR  # noqa: E402
from harness import AccessibilityBridgeTestCase  # noqa: E402
from harness import EventCollector  # noqa: E402
from harness import LadybirdContext  # noqa: E402
from harness import find_first_by_role  # noqa: E402
from harness import role_path  # noqa: E402
from harness import wait_for  # noqa: E402
from harness import wait_for_descendant_by_role  # noqa: E402
from harness import walk  # noqa: E402


class DocumentRootTests(AccessibilityBridgeTestCase):
    """Basic tree-presence invariants: document root exists and is reachable."""

    FIXTURE = "roles.html"

    def test_document_root_has_role_document_web(self):
        """The document web accessible exists with the correct role."""
        self.assertEqual(self.doc.get_role_name(), "document web")

    def test_document_root_parent_is_not_null(self):
        """Document root has a valid parent (the WebContentView accessible)."""
        parent = self.doc.get_parent()
        self.assertIsNotNone(parent)

    def test_document_root_advertises_component(self):
        """Component interface needed for scroll_into_view routing."""
        from harness import supports_interface

        self.assertTrue(supports_interface(self.doc, "component"))


class FocusLeavesEveryElementTests(AccessibilityBridgeTestCase):
    """Focus leaving every element lands on the document — not on the body's section.

    The page's link takes focus through its setFocus action and drops it again half a second later (focus_blur.html).
    The bridge then posts the focus on the document, as Gecko does: the body — the element activeElement falls back
    to — isn't focused, so no focus event names its section, and Orca's locus of focus goes to the document."""

    FIXTURE = "focus_blur.html"

    def _focused_paths(self):
        """The role paths of every accessible under (and including) the document that has the focused state."""
        paths = []

        def visit(obj, _depth):
            if obj.get_state_set().contains(Atspi.StateType.FOCUSED):
                paths.append(role_path(obj))

        walk(self.doc, visit)
        return paths

    def test_blur_lands_on_the_document(self):
        link = find_first_by_role(self.doc, "link")
        self.assertIsNotNone(link, "focus_blur.html must expose a link")
        names = [Atspi.Action.get_action_name(link, i) for i in range(Atspi.Action.get_n_actions(link))]
        index = next((i for i, name in enumerate(names) if "focus" in name.lower()), None)
        self.assertIsNotNone(index, f"the link advertises no setFocus action; actions: {names}")

        collector = EventCollector("object:state-changed:focused")
        self.addCleanup(collector.close)
        collector.pump(0.5)

        self.assertTrue(Atspi.Action.do_action(link, index), "the setFocus action failed")
        focused = wait_for(
            lambda: link.get_state_set().contains(Atspi.StateType.FOCUSED), description="the link to take focus"
        )
        self.assertTrue(focused, "the link never reported the focused state after its setFocus action")
        blurred = wait_for(
            lambda: not link.get_state_set().contains(Atspi.StateType.FOCUSED), description="the link to drop focus"
        )
        self.assertTrue(blurred, "the link kept the focused state; the fixture's blur never ran")
        collector.pump(1.0)

        gained = [(role, name) for _type, detail1, role, name in collector.events if detail1 == 1]
        roles = [role for role, _name in gained]
        self.assertIn("link", roles, f"no focus event named the link; events: {collector.events}")
        self.assertNotIn("section", roles, f"a focus event named a section, the body's; events: {collector.events}")
        self.assertEqual(
            roles[-1],
            "document web",
            f"the last focus event named {gained[-1]}, not the document; events: {collector.events}",
        )

        landed = wait_for(
            lambda: self._focused_paths() == [role_path(self.doc)], description="focus to land on the document"
        )
        self.assertTrue(
            landed, f"after the blur, the focused accessibles are {self._focused_paths()}, not just the document"
        )


class FocusIntoIframeTests(AccessibilityBridgeTestCase):
    """Focus moving into a same-process iframe lands on the iframe element — the tree has no deeper node for it.

    iframe_focus.html hands the focus from its top link on into a srcdoc iframe's link. The tree covers the top-level
    document only, so the iframe document's own focus reports (its focused link, then its root) name nodes the tree
    doesn't have; Document::set_active_element() leaves the reporting to the top-level document, which focuses the
    iframe element in the same pass. So the iframe element ends up with the focused state, rather than nothing at
    all."""

    FIXTURE = "iframe_focus.html"

    def test_focus_into_iframe_lands_on_the_iframe_element(self):
        link = wait_for_descendant_by_role(self.doc, "link", name="Top link")
        self.assertIsNotNone(link, "iframe_focus.html must expose its top link")
        frame = wait_for_descendant_by_role(self.doc, "section", name="Frame")
        self.assertIsNotNone(frame, "iframe_focus.html must expose its iframe as a section named Frame")
        names = [Atspi.Action.get_action_name(link, i) for i in range(Atspi.Action.get_n_actions(link))]
        index = next((i for i, name in enumerate(names) if "focus" in name.lower()), None)
        self.assertIsNotNone(index, f"the link advertises no setFocus action; actions: {names}")

        self.assertTrue(Atspi.Action.do_action(link, index), "the setFocus action failed")
        focused = wait_for(
            lambda: frame.get_state_set().contains(Atspi.StateType.FOCUSED),
            timeout=5.0,
            description="the iframe element to take focus",
        )
        self.assertTrue(focused, "the iframe element never reported the focused state after focus moved into it")
        self.assertFalse(
            link.get_state_set().contains(Atspi.StateType.FOCUSED),
            "the top link kept the focused state after handing the focus on",
        )


class _FixtureServer(http.server.ThreadingHTTPServer):
    """Serves the fixture directory on 127.0.0.1, and records the load late_reader.html reports to /loaded."""

    def __init__(self):
        self.loaded = threading.Event()
        loaded = self.loaded

        class Handler(http.server.SimpleHTTPRequestHandler):
            def __init__(self, *args, **kwargs):
                super().__init__(*args, directory=str(FIXTURE_DIR), **kwargs)

            def log_message(self, *args):
                pass

            def do_GET(self):
                if self.path == "/loaded":
                    loaded.set()
                    self.send_response(204)
                    self.end_headers()
                    return
                super().do_GET()

        super().__init__(("127.0.0.1", 0), Handler)
        threading.Thread(target=self.serve_forever, daemon=True).start()

    def url(self, fixture: str) -> str:
        return f"http://127.0.0.1:{self.server_address[1]}/{fixture}"

    def close(self) -> None:
        self.shutdown()
        self.server_close()


def _set_at_spi_enabled(enabled: bool) -> None:
    """Sets IsEnabled on the session's org.a11y.Status, as a desktop's accessibility setting or a screen reader does."""
    subprocess.run(
        [
            "gdbus",
            "call",
            "--session",
            "--dest",
            "org.a11y.Bus",
            "--object-path",
            "/org/a11y/bus",
            "--method",
            "org.freedesktop.DBus.Properties.Set",
            "org.a11y.Status",
            "IsEnabled",
            "<true>" if enabled else "<false>",
        ],
        check=True,
        capture_output=True,
    )


class AssistiveTechnologyArrivingAfterLoadTests(unittest.TestCase):
    """An assistive technology that turns up after the page loaded gets the page's tree, and hears where the page's
    focus is — but no load announcement.

    Ladybird fetches no tree while Qt's accessibility is off, and Qt turns it on only once AT-SPI reports an assistive
    technology: org.a11y.Status's IsEnabled, or the ScreenReaderEnabled that starting Orca sets. So this test launches
    Ladybird without QT_LINUX_ACCESSIBILITY_ALWAYS_ON, waits for the page to report its load to the test's HTTP server,
    and only then sets IsEnabled. The tree then arrives, and since the page has the keyboard focus, a focus event names
    the page's focused text field — but none names the document, whose focus event is what announces a load."""

    def test_tree_and_focus_arrive_without_a_load_announcement(self):
        if shutil.which("gdbus") is None:
            self.skipTest("needs gdbus to set org.a11y.Status")
        server = _FixtureServer()
        self.addCleanup(server.close)
        ctx = LadybirdContext(server.url("late_reader.html"), accessibility_at_launch=False)
        ctx.start()
        self.addCleanup(ctx.stop)

        self.assertTrue(server.loaded.wait(60), "late_reader.html never reported its load")
        # The page reports its load from its load event, which fires only after WebContent has told the UI process the
        # load finished. Give the UI process a moment to act on that before accessibility comes on.
        time.sleep(0.5)
        if ctx.find_app_on_desktop() is not None:
            self.skipTest("accessibility is already on in this session, so there's no turning it on after the load")

        collector = EventCollector("object:state-changed:focused")
        self.addCleanup(collector.close)
        collector.pump(0.5)
        _set_at_spi_enabled(True)
        self.addCleanup(_set_at_spi_enabled, False)

        app = ctx.wait_for_app()
        self.assertIsNotNone(app, "setting IsEnabled never brought Ladybird onto the AT-SPI2 desktop")

        def populated_document():
            doc = LadybirdContext.find_document_web(app)
            return doc if doc is not None and doc.get_child_count() > 0 else None

        doc = wait_for(populated_document, description="the page's tree")
        self.assertIsNotNone(doc, "turning accessibility on after the load never brought the page's tree")
        self.assertIsNotNone(find_first_by_role(doc, "heading"), "the page's tree arrived without the page's heading")

        # A page load's own focus event on the document comes a second after its first tree, so wait past that.
        collector.pump(2.0)
        gained = [(role, name) for _type, detail1, role, name in collector.events if detail1 == 1]
        self.assertIn(
            ("text", "Name"), gained, f"no focus event named the page's text field; the events were {collector.events}"
        )
        self.assertNotIn(
            "document web",
            [role for role, _name in gained],
            f"a focus event on the document announced a load nobody was there for; the events were {collector.events}",
        )


if __name__ == "__main__":
    unittest.main()
