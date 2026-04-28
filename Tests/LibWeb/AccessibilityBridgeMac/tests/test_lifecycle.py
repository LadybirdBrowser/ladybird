"""Lifecycle invariants — page-load focus, AXWebArea presence, basic tree shape."""

from __future__ import annotations

import http.server
import pathlib
import sys
import threading
import time
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from AppKit import NSRunningApplication  # noqa: E402
from AppKit import NSWorkspace  # noqa: E402
from ApplicationServices import AXUIElementCreateApplication  # noqa: E402
from ApplicationServices import AXUIElementSetAttributeValue  # noqa: E402
from ApplicationServices import kAXChildrenAttribute  # noqa: E402
from ApplicationServices import kAXFocusedAttribute  # noqa: E402
from ApplicationServices import kAXFocusedUIElementAttribute  # noqa: E402
from ApplicationServices import kAXFrontmostAttribute  # noqa: E402
from ApplicationServices import kAXRoleAttribute  # noqa: E402
from ApplicationServices import kAXTitleAttribute  # noqa: E402
from CoreFoundation import CFRunLoopRunInMode  # noqa: E402
from CoreFoundation import kCFRunLoopDefaultMode  # noqa: E402
from harness import FIXTURE_DIR  # noqa: E402
from harness import AccessibilityBridgeMacTestCase  # noqa: E402
from harness import LadybirdContext  # noqa: E402
from harness import NotificationCollector  # noqa: E402
from harness import find_first_by_role  # noqa: E402
from harness import wait_for  # noqa: E402
from harness import wait_for_descendant_by_role  # noqa: E402
from harness.ladybird import _ax_attr  # noqa: E402
from harness.ladybird import _find_by_role  # noqa: E402


class WebAreaPresenceTests(AccessibilityBridgeMacTestCase):
    """The AXWebArea exists and has children after page load."""

    FIXTURE = "roles.html"

    def test_web_area_exists(self):
        """The AXWebArea (document root) is reachable from the application."""
        self.assertIsNotNone(self.web)
        self.assertEqual(_ax_attr(self.web, kAXRoleAttribute), "AXWebArea")

    def test_web_area_has_children(self):
        """The AXWebArea has at least one child accessibility element."""
        children = _ax_attr(self.web, kAXChildrenAttribute) or []
        self.assertGreater(
            len(children),
            0,
            "AXWebArea has no children — accessibility tree did not populate",
        )


class FocusLeavesEveryElementTests(AccessibilityBridgeMacTestCase):
    """Focus leaving every element lands on the AXWebArea — not on the body's group.

    The page's link takes focus (AXFocused=YES moves DOM focus there, and the web view takes first responder with it)
    and drops it again half a second later (focus_blur.html). Once it has, the application's AXFocusedUIElement is the
    AXWebArea, and every AXFocusedUIElementChanged after the link's names the web area — AppKit posts that one itself
    once it's told the focus changed, as it does for WebKit — and none names the body, the element activeElement falls
    back to, which isn't focused. Whatever precedes the link's isn't the blur's doing."""

    FIXTURE = "focus_blur.html"

    def test_blur_lands_on_the_web_area(self):
        collector = NotificationCollector(self.ctx.pid, self.app, ["AXFocusedUIElementChanged"])
        try:
            # Let anything still in flight from the initial load land before the experiment starts.
            collector.collect(1.0)
            collector.clear()

            link = wait_for_descendant_by_role(self.web, "AXLink")
            self.assertIsNotNone(link, "focus_blur.html must expose an AXLink")
            err = AXUIElementSetAttributeValue(link, kAXFocusedAttribute, True)
            self.assertEqual(err, 0, "setting AXFocused on the link failed")

            def link_focused():
                collector.collect(0.1)
                return bool(_ax_attr(link, kAXFocusedAttribute))

            self.assertTrue(
                wait_for(link_focused, description="the link to take focus"), "the link never reported AXFocused"
            )
            self.assertTrue(
                wait_for(lambda: not link_focused(), description="the link to drop focus"),
                "the link kept AXFocused; the fixture's blur never ran",
            )
            # Give the blur's notifications time to arrive.
            collector.collect(1.0)
        finally:
            collector.close()

        roles = [_ax_attr(elem, kAXRoleAttribute) for elem in collector.elements_for("AXFocusedUIElementChanged")]
        self.assertIn("AXLink", roles, f"no AXFocusedUIElementChanged for the link; notifications were for {roles}")
        after_link = roles[len(roles) - roles[::-1].index("AXLink") :]
        self.assertEqual(
            [role for role in after_link if role != "AXWebArea"],
            [],
            "AXFocusedUIElementChanged named something other than the web area after the link's; "
            f"notifications were for {roles}",
        )
        focused = _ax_attr(self.app, kAXFocusedUIElementAttribute)
        self.assertIsNotNone(focused, "the application reports no focused UI element after the blur")
        self.assertEqual(
            _ax_attr(focused, kAXRoleAttribute),
            "AXWebArea",
            f"after the blur the focused UI element is {_ax_attr(focused, kAXRoleAttribute)}, not the AXWebArea",
        )


class FocusIntoIframeTests(AccessibilityBridgeMacTestCase):
    """Focus moving into a same-process iframe lands on the iframe element — never back on the AXWebArea.

    iframe_focus.html hands the focus from its top link on into a srcdoc iframe's link. The tree covers the top-level
    document only, so the iframe document's own focus reports (its focused link, then its root) name nodes the tree
    doesn't have; Document::set_active_element() leaves the reporting to the top-level document, which focuses the
    iframe element in the same pass. So every AXFocusedUIElementChanged after the link's names the iframe element,
    and it is the application's AXFocusedUIElement: VoiceOver isn't sent back to the top of the page."""

    FIXTURE = "iframe_focus.html"

    def test_focus_into_iframe_lands_on_the_iframe_element(self):
        collector = NotificationCollector(self.ctx.pid, self.app, ["AXFocusedUIElementChanged"])
        try:
            # Let anything still in flight from the initial load land before the experiment starts.
            collector.collect(1.0)
            collector.clear()

            link = wait_for_descendant_by_role(self.web, "AXLink", title="Top link")
            self.assertIsNotNone(link, "iframe_focus.html must expose its top link")
            err = AXUIElementSetAttributeValue(link, kAXFocusedAttribute, True)
            self.assertEqual(err, 0, "setting AXFocused on the link failed")

            def link_focused():
                collector.collect(0.1)
                return bool(_ax_attr(link, kAXFocusedAttribute))

            self.assertTrue(
                wait_for(link_focused, description="the link to take focus"), "the link never reported AXFocused"
            )
            self.assertTrue(
                wait_for(lambda: not link_focused(), description="the link to hand the focus on"),
                "the link kept AXFocused; the fixture never moved the focus into the iframe",
            )
            # Give the iframe's focus reports time to arrive.
            collector.collect(1.0)
        finally:
            collector.close()

        roles = [_ax_attr(elem, kAXRoleAttribute) for elem in collector.elements_for("AXFocusedUIElementChanged")]
        self.assertIn("AXLink", roles, f"no AXFocusedUIElementChanged for the link; notifications were for {roles}")
        after_link = roles[len(roles) - roles[::-1].index("AXLink") :]
        self.assertNotIn(
            "AXWebArea",
            after_link,
            "AXFocusedUIElementChanged named the web area after the link's: the iframe's focus reports pulled the "
            f"focus off every node; notifications were for {roles}",
        )
        focused = _ax_attr(self.app, kAXFocusedUIElementAttribute)
        self.assertIsNotNone(focused, "the application reports no focused UI element after the focus moved")
        self.assertEqual(
            (_ax_attr(focused, kAXRoleAttribute), _ax_attr(focused, kAXTitleAttribute)),
            ("AXGroup", "Frame"),
            "after the focus moved into the iframe, the focused UI element isn't the iframe element",
        )


def _bring_to_front(app) -> bool:
    """Makes an application the frontmost one, as VoiceOver does with the app its user works in. macOS can turn that
    down (while someone's typing into another app, e.g.), so this asks again every second, for up to 10s. Returns
    whether the application came to the front."""
    for _ in range(10):
        AXUIElementSetAttributeValue(app, kAXFrontmostAttribute, True)
        if wait_for(lambda: bool(_ax_attr(app, kAXFrontmostAttribute)), timeout=1.0, description="the app in front"):
            return True
    return False


def _frontmost_app():
    """The frontmost application's AXUIElement, or None."""
    # NSWorkspace learns of activations through the run loop, so let that run first.
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, False)
    running = NSWorkspace.sharedWorkspace().frontmostApplication()
    return AXUIElementCreateApplication(running.processIdentifier()) if running is not None else None


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


class _ReaderArrivesAfterLoad(unittest.TestCase):
    """Launches Ladybird on late_reader.html, and waits for the load to finish without touching the app's accessibility.

    A web view fetches its tree only once Qt's accessibility is active, which the first accessibility query into a Qt
    window turns on (-[QNSView activateQtAccessibility]) — any query, the window's title included. So these tests make
    none until the page has loaded: They serve the fixture over HTTP and wait for the load it reports to the server.
    The first query a test then makes is the one that turns Qt's accessibility on, as an assistive technology started
    after the load would.

    Ladybird has to be in the background, as on a runner, where nothing brings a launched app to the front: a web view
    that holds the keyboard focus in the key window reports the focus to a reader that turns up. Qt activates the app
    on launch on some machines, so the launch hands the front back to whichever app had it."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        if NSWorkspace.sharedWorkspace().isVoiceOverEnabled():
            raise unittest.SkipTest("VoiceOver is on, so the web view fetches its tree while the page loads")
        previous = _frontmost_app()
        cls._server = _FixtureServer()
        cls._ctx = LadybirdContext(cls._server.url("late_reader.html"), wait_for_web_area=False)
        cls._ctx.start()
        if not cls._server.loaded.wait(timeout=30):
            cls._ctx.stop()
            cls._server.close()
            raise RuntimeError("late_reader.html never reported its load; the page didn't load")
        # The load event has fired; give WebContent's load-finish report to the UI a moment to land too.
        time.sleep(0.5)
        # NSRunningApplication answers this without an accessibility query into Ladybird; it learns of activations
        # through the run loop, so that runs first.
        running = NSRunningApplication.runningApplicationWithProcessIdentifier_(cls._ctx.pid)

        def ladybird_is_active():
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, False)
            return running is not None and running.isActive()

        if ladybird_is_active() and previous is not None:
            _bring_to_front(previous)
            if not wait_for(lambda: not ladybird_is_active(), description="Ladybird to leave the front"):
                cls._ctx.stop()
                cls._server.close()
                raise unittest.SkipTest("Ladybird started as the active app, and stayed there")

    @classmethod
    def tearDownClass(cls):
        ctx = getattr(cls, "_ctx", None)
        if ctx is not None:
            ctx.stop()
        server = getattr(cls, "_server", None)
        if server is not None:
            server.close()
        super().tearDownClass()

    @property
    def ctx(self) -> LadybirdContext:
        return type(self)._ctx

    def populated_web_area(self):
        """The document root once it has children, else None."""
        web = self.ctx.find_web_area()
        if web is None or not (_ax_attr(web, kAXChildrenAttribute) or []):
            return None
        return web


class ReaderArrivingAfterLoadTests(_ReaderArrivesAfterLoad):
    """An assistive technology that first reads a page after its load gets the tree — but no load announcement.

    Nobody was reading while the page loaded, so the web view didn't fetch its tree: The first read turns Qt's
    accessibility on, which fetches it. No AXLoadComplete or AXFocusedUIElementChanged comes with that tree: The page
    doesn't pull a reader that turns up later away from wherever it is, and the keyboard focus stays where the browser
    put it."""

    def test_first_read_after_load_fetches_the_tree(self):
        app = self.ctx.app
        collector = NotificationCollector(self.ctx.pid, app, ["AXLoadComplete", "AXFocusedUIElementChanged"])
        try:
            web = wait_for(self.populated_web_area, description="the page's tree")
            self.assertIsNotNone(web, "reading into the page never brought its tree")
            self.assertIsNotNone(
                find_first_by_role(web, "AXHeading"), "the page's tree arrived without the page's heading"
            )
            collector.collect(1.0)
        finally:
            collector.close()

        self.assertEqual(collector.names(), [], "the first read into a loaded page announced a load or moved the focus")


class ReaderMovingFocusIntoLoadedPageTests(_ReaderArrivesAfterLoad):
    """An assistive technology that arrives after the load, and moves the focus into the page, hears where the page's
    focus is: on the text field the page focused while it loaded.

    The reader brings Ladybird to the front first, as VoiceOver does with the app its user works in: A web view reports
    the focus only while it's the key window's first responder, as in Chrome. It then focuses the web view's element
    (the AXWebArea Qt's element for the widget presents as), which gives the widget the keyboard focus. The first query
    into the window had turned Qt's accessibility on, and the tree that fetched arrives without a focused element to
    report until it's there; once it is, the view has AppKit look the focus up again and post AXFocusedUIElementChanged
    for the text field."""

    def test_focus_moved_into_the_page_lands_on_the_page_focus(self):
        app = self.ctx.app
        # Qt's element for the web view: the first AXWebArea under the window (the document root is the one inside it).
        view = _find_by_role(self.ctx.window(), "AXWebArea")
        self.assertIsNotNone(view, "no web view (AXWebArea) in the window")

        # Hand the front back to whichever app had it, so that the next test's Ladybird doesn't start as the active app.
        previous = _frontmost_app()
        if previous is not None:
            self.addCleanup(_bring_to_front, previous)

        collector = NotificationCollector(self.ctx.pid, app, ["AXLoadComplete", "AXFocusedUIElementChanged"])
        try:
            if not _bring_to_front(app):
                self.skipTest("macOS didn't let the test bring Ladybird to the front")
            collector.collect(0.5)

            # The window that became key may have put the focus in the web view already, which makes this a no-op. It's
            # for a window that hands the focus to its address bar instead.
            err = AXUIElementSetAttributeValue(view, kAXFocusedAttribute, True)
            self.assertEqual(err, 0, "setting AXFocused on the web view failed")

            def field_focused():
                collector.collect(0.1)
                focused = _ax_attr(app, kAXFocusedUIElementAttribute)
                return _ax_attr(focused, kAXTitleAttribute) == "Name"

            self.assertTrue(
                wait_for(field_focused, description="the page's text field to be the focused element"),
                "the page's focused text field never became the application's focused element",
            )
            collector.collect(1.0)
        finally:
            collector.close()

        self.assertNotIn("AXLoadComplete", collector.names(), "moving the focus into a loaded page announced a load")
        focus_changes = [
            (_ax_attr(elem, kAXRoleAttribute), _ax_attr(elem, kAXTitleAttribute))
            for elem in collector.elements_for("AXFocusedUIElementChanged")
        ]
        self.assertTrue(focus_changes, "no AXFocusedUIElementChanged arrived")
        self.assertEqual(
            focus_changes[-1],
            ("AXTextField", "Name"),
            f"the last AXFocusedUIElementChanged didn't name the page's text field; they named {focus_changes}",
        )


if __name__ == "__main__":
    unittest.main()
