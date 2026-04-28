"""Lifecycle invariants — page-load focus, AXWebArea presence, basic tree shape."""

from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from AppKit import NSWorkspace  # noqa: E402
from ApplicationServices import AXUIElementCreateApplication  # noqa: E402
from ApplicationServices import AXUIElementSetAttributeValue  # noqa: E402
from ApplicationServices import kAXChildrenAttribute  # noqa: E402
from ApplicationServices import kAXFocusedAttribute  # noqa: E402
from ApplicationServices import kAXFocusedUIElementAttribute  # noqa: E402
from ApplicationServices import kAXFrontmostAttribute  # noqa: E402
from ApplicationServices import kAXParentAttribute  # noqa: E402
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


def _inside_web_area(element) -> bool:
    """Whether element is the AXWebArea or one of its descendants: the focus is in the page, not in the browser UI."""
    for _ in range(50):
        if element is None:
            return False
        if _ax_attr(element, kAXRoleAttribute) == "AXWebArea":
            return True
        element = _ax_attr(element, kAXParentAttribute)
    return False


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


class _ReaderArrivesAfterLoad(unittest.TestCase):
    """Launches Ladybird on late_reader.html without reading into the page, and waits for the load to finish.

    A web view fetches its tree only once an assistive technology reads into web content, or VoiceOver is on when the
    page loads. So these tests read nothing from the web view until the page has loaded: They watch for the load
    through the window title (which the page sets once it's loaded, and which AppKit answers without asking the web
    view), and only then read into the page, as an assistive technology started after the load would.

    Ladybird has to start in the background, as it does when a test launches it: A window that becomes key hands the
    keyboard focus to the web view, and the first query for the focused element then fetches the page's tree."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        if NSWorkspace.sharedWorkspace().isVoiceOverEnabled():
            raise unittest.SkipTest("VoiceOver is on, so the web view fetches its tree while the page loads")
        cls._ctx = LadybirdContext((FIXTURE_DIR / "late_reader.html").resolve().as_uri(), wait_for_web_area=False)
        cls._ctx.start()

        def loaded():
            window = cls._ctx.window()
            return window is not None and _ax_attr(window, kAXTitleAttribute) == "Loaded"

        if not wait_for(loaded, timeout=30, description="the page to load"):
            cls._ctx.stop()
            raise RuntimeError("late_reader.html never retitled its window; the page didn't load")
        if _ax_attr(cls._ctx.app, kAXFrontmostAttribute):
            cls._ctx.stop()
            raise unittest.SkipTest("Ladybird started as the active app, so its window gave the web view the focus")

    @classmethod
    def tearDownClass(cls):
        ctx = getattr(cls, "_ctx", None)
        if ctx is not None:
            ctx.stop()
        super().tearDownClass()

    @property
    def ctx(self) -> LadybirdContext:
        return type(self)._ctx

    def populated_web_area(self):
        """The AXWebArea once it has children, else None. Reading the window's children reads into the web view."""
        web = self.ctx.find_web_area()
        if web is None or not (_ax_attr(web, kAXChildrenAttribute) or []):
            return None
        return web


class ReaderArrivingAfterLoadTests(_ReaderArrivesAfterLoad):
    """An assistive technology that first reads a page after its load gets the tree — but no load announcement.

    Nobody was reading while the page loaded, so the web view didn't fetch its tree: The first read finds none, and
    fetches it. The keyboard focus stays where the browser put it (the address bar), and no AXLoadComplete arrives: The
    page doesn't pull a reader that turns up later away from wherever it is."""

    def test_first_read_after_load_fetches_the_tree(self):
        app = self.ctx.app
        # The search stops at the web view (an AXScrollArea) without reading its children.
        view = _find_by_role(self.ctx.window(), "AXScrollArea")
        self.assertIsNotNone(view, "no web view (AXScrollArea) in the window")

        collector = NotificationCollector(self.ctx.pid, app, ["AXLoadComplete", "AXFocusedUIElementChanged"])
        try:
            self.assertFalse(
                _ax_attr(view, kAXChildrenAttribute), "the web view had fetched its tree before anything read into it"
            )
            web = wait_for(self.populated_web_area, description="the page's tree")
            self.assertIsNotNone(web, "reading into the page never brought its tree")
            self.assertIsNotNone(
                find_first_by_role(web, "AXHeading"), "the page's tree arrived without the page's heading"
            )
            collector.collect(1.0)
        finally:
            collector.close()

        self.assertEqual(collector.names(), [], "the first read into a loaded page announced a load or moved the focus")
        focused = _ax_attr(app, kAXFocusedUIElementAttribute)
        self.assertIsNotNone(focused, "the application reports no focused UI element")
        self.assertFalse(_inside_web_area(focused), "the keyboard focus moved into the page with nobody asking for it")


class ReaderMovingFocusIntoLoadedPageTests(_ReaderArrivesAfterLoad):
    """An assistive technology that arrives after the load, and moves the focus into the page, hears where the page's
    focus is: on the text field the page focused while it loaded.

    The reader brings Ladybird to the front first, as VoiceOver does with the app its user works in: A web view reports
    the focus only while it's the key window's first responder, as in Chrome. The window that becomes key hands the
    keyboard focus to the web view, and AppKit then asks the web view for its focused element before its tree exists —
    so the answer is the web view itself. Once the tree arrives, AppKit looks the focus up again and posts
    AXFocusedUIElementChanged for the text field."""

    def test_focus_moved_into_the_page_lands_on_the_page_focus(self):
        app = self.ctx.app
        # The search stops at the web view (an AXScrollArea) without reading its children.
        view = _find_by_role(self.ctx.window(), "AXScrollArea")
        self.assertIsNotNone(view, "no web view (AXScrollArea) in the window")

        # Hand the front back to whichever app had it, so that the next test's Ladybird doesn't start as the active app.
        previous = _frontmost_app()
        if previous is not None:
            self.addCleanup(_bring_to_front, previous)

        collector = NotificationCollector(self.ctx.pid, app, ["AXLoadComplete", "AXFocusedUIElementChanged"])
        try:
            if not _bring_to_front(app):
                self.skipTest("macOS didn't let the test bring Ladybird to the front")
            collector.collect(0.5)

            # The window that became key has put the focus in the web view already, which makes this a no-op. It's for
            # a window that hands the focus to its address bar instead.
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
