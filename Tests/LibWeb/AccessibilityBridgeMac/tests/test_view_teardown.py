"""An element an assistive technology holds outlives the view it came from."""

from __future__ import annotations

import pathlib
import sys
import time
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from AppKit import NSApplicationActivateIgnoringOtherApps  # noqa: E402
from AppKit import NSRunningApplication  # noqa: E402
from AppKit import NSWorkspace  # noqa: E402
from ApplicationServices import kAXRoleAttribute  # noqa: E402
from ApplicationServices import kAXTitleAttribute  # noqa: E402
from harness import FIXTURE_DIR  # noqa: E402
from harness import LadybirdContext  # noqa: E402
from harness import wait_for  # noqa: E402
from harness import wait_for_descendant_by_role  # noqa: E402
from harness.ladybird import _ax_attr  # noqa: E402
from Quartz import kCGEventFlagMaskCommand  # noqa: E402

from tests.test_native_control_key_editing import send_native_key  # noqa: E402

KEY_CODE_1 = 18
KEY_CODE_T = 17
KEY_CODE_W = 13


class HeldElementOutlivesItsViewTests(unittest.TestCase):
    """Regression: an element of a closed tab crashed Ladybird when an assistive technology read it.

    The elements of a web view hold a raw pointer into the view's accessibility tree, which goes away with the view.
    An element the tree dropped gives the pointer up, but one that was still in the tree when its tab closed kept it,
    and the next attribute an assistive technology asked of it read the freed tree (a SIGBUS in
    AccessibilityTreeManager::node()). Now the view's going away invalidates every element its overlay vended."""

    def setUp(self):
        super().setUp()
        self._previous_application = NSWorkspace.sharedWorkspace().frontmostApplication()
        self.ctx = LadybirdContext((FIXTURE_DIR / "sibling_insert.html").as_uri(), launch_via_launch_services=True)
        self.addCleanup(self._cleanup)
        self.ctx.start()

    def _cleanup(self):
        self.ctx.stop()
        if self._previous_application:
            self._previous_application.activateWithOptions_(NSApplicationActivateIgnoringOtherApps)

    def test_reading_an_element_of_a_closed_tab_does_not_crash(self):
        beta = wait_for_descendant_by_role(self.ctx.web, "AXButton", title="Beta")
        self.assertIsNotNone(beta, "sibling_insert.html must expose its Beta button")
        self.assertEqual(_ax_attr(beta, kAXTitleAttribute), "Beta")

        # A second tab keeps the window open once the first one closes.
        send_native_key(self.ctx.pid, KEY_CODE_T, kCGEventFlagMaskCommand)
        time.sleep(1.0)
        send_native_key(self.ctx.pid, KEY_CODE_1, kCGEventFlagMaskCommand)
        time.sleep(0.5)
        send_native_key(self.ctx.pid, KEY_CODE_W, kCGEventFlagMaskCommand)

        # Once the first tab's view is gone, its element answers nothing, and Ladybird keeps running.
        def element_is_gone():
            return _ax_attr(beta, kAXRoleAttribute) is None

        self.assertTrue(wait_for(element_is_gone, timeout=10.0), "the closed tab's element still answers")
        for _ in range(20):
            _ax_attr(beta, kAXTitleAttribute)
            _ax_attr(beta, kAXRoleAttribute)
            time.sleep(0.05)
        application = NSRunningApplication.runningApplicationWithProcessIdentifier_(self.ctx.pid)
        self.assertTrue(
            application is not None and not application.isTerminated(),
            "Ladybird exited after an element of a closed tab was read",
        )


if __name__ == "__main__":
    unittest.main()
