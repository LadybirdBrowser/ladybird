"""Native Control-key events exercise AppKit bindings through the Qt view and WebContent."""

from __future__ import annotations

import json
import pathlib
import plistlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from AppKit import NSApplicationActivateIgnoringOtherApps  # noqa: E402
from AppKit import NSRunningApplication  # noqa: E402
from AppKit import NSWorkspace  # noqa: E402
from ApplicationServices import AXUIElementPerformAction  # noqa: E402
from ApplicationServices import AXUIElementSetAttributeValue  # noqa: E402
from ApplicationServices import kAXFocusedAttribute  # noqa: E402
from ApplicationServices import kAXFocusedUIElementAttribute  # noqa: E402
from ApplicationServices import kAXPressAction  # noqa: E402
from ApplicationServices import kAXRoleAttribute  # noqa: E402
from ApplicationServices import kAXSelectedTextAttribute  # noqa: E402
from ApplicationServices import kAXTitleAttribute  # noqa: E402
from ApplicationServices import kAXValueAttribute  # noqa: E402
from Foundation import NSDate  # noqa: E402
from Foundation import NSRunLoop  # noqa: E402
from harness import find_all_by_role  # noqa: E402
from harness import wait_for  # noqa: E402
from harness.ladybird import FIXTURE_DIR  # noqa: E402
from harness.ladybird import LadybirdContext  # noqa: E402
from harness.ladybird import _ax_attr  # noqa: E402
from Quartz import CGEventCreateKeyboardEvent  # noqa: E402
from Quartz import CGEventPostToPid  # noqa: E402
from Quartz import CGEventSetFlags  # noqa: E402
from Quartz import kCGEventFlagMaskAlternate  # noqa: E402
from Quartz import kCGEventFlagMaskCommand  # noqa: E402
from Quartz import kCGEventFlagMaskControl  # noqa: E402
from Quartz import kCGEventFlagMaskShift  # noqa: E402


def activate_fixture(pid: int) -> None:
    def running_application():
        NSRunLoop.currentRunLoop().runUntilDate_(NSDate.dateWithTimeIntervalSinceNow_(0.01))
        return NSRunningApplication.runningApplicationWithProcessIdentifier_(pid)

    application = wait_for(running_application)
    if application is None or application is False:
        raise AssertionError("The native-event fixture is not registered with LaunchServices")
    application.activateWithOptions_(NSApplicationActivateIgnoringOtherApps)

    def is_active():
        NSRunLoop.currentRunLoop().runUntilDate_(NSDate.dateWithTimeIntervalSinceNow_(0.01))
        return NSWorkspace.sharedWorkspace().frontmostApplication().processIdentifier() == pid

    if not wait_for(is_active):
        raise AssertionError("The native-event fixture couldn't become the active application")


def send_native_key(pid: int, key_code: int, flags: int = 0) -> None:
    activate_fixture(pid)
    for down in (True, False):
        event = CGEventCreateKeyboardEvent(None, key_code, down)
        CGEventSetFlags(event, flags)
        CGEventPostToPid(pid, event)


def send_native_control_key(pid: int, key_code: int, *, shift: bool = False, option: bool = False) -> None:
    flags = kCGEventFlagMaskControl
    if shift:
        flags |= kCGEventFlagMaskShift
    if option:
        flags |= kCGEventFlagMaskAlternate
    send_native_key(pid, key_code, flags)


class NativeControlKeyEditingTests(unittest.TestCase):
    MODE = "default"

    def setUp(self):
        super().setUp()
        self._home = tempfile.TemporaryDirectory(prefix="ladybird-native-ui-bindings-")
        bindings = pathlib.Path(self._home.name) / "Library" / "KeyBindings"
        bindings.mkdir(parents=True)
        dictionary = (
            {}
            if self.MODE == "default"
            else {
                "^w": "deleteWordBackward:",
                "^g": "deleteWordBackward:",
                "^b": "moveWordBackward:",
                "^f": "moveWordForward:",
                "^s": "moveBackwardAndModifySelection:",
                "^z": ["moveToBeginningOfParagraph:", "moveForward:"],
                "^u": ["moveBackward:", "transpose:"],
                "^" + chr(0xF702): "noop:",
            }
        )
        (bindings / "DefaultKeyBinding.dict").write_bytes(plistlib.dumps(dictionary))
        self._previous_application = NSWorkspace.sharedWorkspace().frontmostApplication()
        self.ctx = LadybirdContext(
            (FIXTURE_DIR / "native-control-key-editing.html").as_uri(),
            environment={"CFFIXED_USER_HOME": self._home.name},
            launch_via_launch_services=True,
        )
        self.addCleanup(self._cleanup_fixture)
        self.ctx.start()
        activate_fixture(self.ctx.pid)

    def _cleanup_fixture(self):
        self.ctx.stop()
        self._home.cleanup()
        if self._previous_application:
            self._previous_application.activateWithOptions_(NSApplicationActivateIgnoringOtherApps)

    def _button(self, name):
        return next(
            (
                button
                for button in find_all_by_role(self.ctx.web, "AXButton")
                if _ax_attr(button, kAXTitleAttribute) == name
            ),
            None,
        )

    def _result(self):
        for field in find_all_by_role(self.ctx.web, "AXTextField"):
            value = _ax_attr(field, kAXValueAttribute)
            if isinstance(value, str) and value.startswith('{"serial":'):
                return json.loads(value)
        return None

    def _prepare(self, name):
        previous = self._result()
        button = self._button(name)
        self.assertIsNotNone(button, name)
        self.assertEqual(AXUIElementPerformAction(button, kAXPressAction), 0)
        self.assertTrue(
            wait_for(lambda: self._result() is not None and self._result() != previous),
            "fixture did not focus the editor",
        )
        return self._result()

    def _control(self, key_code, *, shift=False):
        before = self._result()
        send_native_control_key(self.ctx.pid, key_code, shift=shift)
        self.assertTrue(
            wait_for(lambda: self._result() is not None and self._result()["serial"] > before["serial"]),
            f"fixture did not receive keyup: {self._result()!r}",
        )
        return self._result()

    def test_web_input_native_commands(self):
        self._prepare("Input")
        state = self._control(0)  # A
        self.assertEqual([state["start"], state["end"]], [0, 0])
        self.assertIn(["keydown", "a", True, False], state["events"])
        state = self._control(14)  # E
        self.assertEqual(state["end"], 11)
        state = self._control(13)  # W
        self.assertEqual(state["value"], "hello " if self.MODE == "custom" else "hello world")
        self._control(0)
        state = self._control(40)  # K
        self.assertEqual(state["value"], "")
        self.assertIn(["beforeinput", "deleteHardLineForward"], state["events"])

    def test_selector_overrides_and_selection(self):
        self._prepare("Input")
        state = self._control(11)  # B
        self.assertEqual(state["end"], 6 if self.MODE == "custom" else 10)
        state = self._control(3)  # F
        self.assertEqual(state["end"], 11)
        state = self._control(0, shift=True)
        self.assertEqual([state["start"], state["end"]], [0, 11])

    def test_paragraph_editors_and_iframe(self):
        self._prepare("Textarea")
        self.assertEqual(self._control(0)["start"], 4)
        self.assertEqual(self._control(40)["value"], "abc\n")
        for name in ("Rich text", "Iframe"):
            with self.subTest(editor=name):
                self._prepare(name)
                self.assertEqual(self._control(0)["end"], 0)
                self.assertEqual(self._control(14)["end"], 11)
                state = self._control(13)
                self.assertEqual(
                    state["value"].replace(chr(0xA0), " "), "hello " if self.MODE == "custom" else "hello world"
                )
                self._control(0)
                self.assertEqual(self._control(40)["value"], "")

    def test_dom_cancellation(self):
        self._prepare("Cancel keydown")
        self.assertEqual(self._control(0)["end"], 11)
        self._prepare("Cancel deletion")
        self._control(0)
        state = self._control(40)
        self.assertEqual([state["value"], state["end"]], ["hello world", 0])
        self.assertNotIn(["input", "deleteHardLineForward"], state["events"])

    def test_supported_sequences_and_rejected_bindings(self):
        if self.MODE != "custom":
            self.skipTest("custom binding fixture")
        self._prepare("Input")
        self.assertEqual(self._control(6)["end"], 1)  # Z: ordered sequence
        self._prepare("Input")
        self.assertEqual([self._control(32)["start"], self._result()["end"]], [11, 11])  # U: unsupported sequence
        self.assertEqual(self._control(123)["end"], 11)  # Left: resolved noop
        self.assertEqual([self._control(1)["start"], self._result()["end"]], [10, 11])  # S: selection without Shift
        self._prepare("Input")
        self.assertEqual(self._control(5)["value"], "hello ")  # G: alternate custom binding

    def _focused_ui_field(self):
        field = _ax_attr(self.ctx.app, kAXFocusedUIElementAttribute)
        return field if _ax_attr(field, kAXRoleAttribute) == "AXTextField" else None

    def _check_ui_commands(self, field):
        self.assertEqual(AXUIElementSetAttributeValue(field, kAXValueAttribute, "hello world"), 0)
        send_native_control_key(self.ctx.pid, 14)  # E
        send_native_control_key(self.ctx.pid, 13)  # W
        expected = "hello " if self.MODE == "custom" else "hello world"
        self.assertTrue(wait_for(lambda: _ax_attr(field, kAXValueAttribute) == expected))
        send_native_control_key(self.ctx.pid, 0)  # A
        send_native_control_key(self.ctx.pid, 40)  # K
        self.assertTrue(wait_for(lambda: _ax_attr(field, kAXValueAttribute) == ""))
        send_native_key(self.ctx.pid, 6, kCGEventFlagMaskCommand)  # Command-Z
        self.assertTrue(wait_for(lambda: _ax_attr(field, kAXValueAttribute) == expected))
        send_native_key(self.ctx.pid, 0, kCGEventFlagMaskCommand)  # Command-A
        send_native_key(self.ctx.pid, 7)  # Ordinary X typing replaces the selection.
        self.assertTrue(wait_for(lambda: _ax_attr(field, kAXValueAttribute) == "x"))

    def test_search_field_native_commands(self):
        send_native_key(self.ctx.pid, 3, kCGEventFlagMaskCommand)  # Command-F
        self.addCleanup(send_native_key, self.ctx.pid, 53)
        field = wait_for(self._focused_ui_field)
        self.assertTrue(field, "search field did not take focus")
        self._check_ui_commands(field)

    def test_prompt_native_commands(self):
        button = self._button("Prompt")
        self.assertIsNotNone(button)
        self.assertEqual(AXUIElementPerformAction(button, kAXPressAction), 0)
        self.addCleanup(send_native_key, self.ctx.pid, 53)
        field = wait_for(self._focused_ui_field)
        self.assertTrue(field, "prompt field did not take focus")
        self._check_ui_commands(field)

    def _leave_address_field(self, address):
        send_native_key(self.ctx.pid, 53)  # Escape closes completion.
        send_native_key(self.ctx.pid, 53)  # Escape returns focus to the page.
        self.assertTrue(wait_for(lambda: not _ax_attr(address, kAXFocusedAttribute)))

    def test_address_completion_native_commands(self):
        send_native_key(self.ctx.pid, 37, kCGEventFlagMaskCommand)  # Command-L
        address = wait_for(self._focused_ui_field)
        self.assertTrue(address, "address field did not take focus")
        self.addCleanup(self._leave_address_field, address)

        def type_internal_page_prefix():
            prefix = ""
            for character, code in zip("about:", (0, 11, 31, 32, 17, 41)):
                prefix += character
                send_native_key(self.ctx.pid, code, kCGEventFlagMaskShift if character == ":" else 0)

                def typed_prefix(prefix=prefix):
                    value = _ax_attr(address, kAXValueAttribute)
                    return (
                        isinstance(value, str)
                        and value.startswith(prefix)
                        and _ax_attr(address, kAXSelectedTextAttribute) == value[len(prefix) :]
                    )

                self.assertTrue(
                    wait_for(typed_prefix), f"prefix after typing {character}: {_ax_attr(address, kAXValueAttribute)!r}"
                )

            def completion():
                value = _ax_attr(address, kAXValueAttribute)
                if not isinstance(value, str) or not value.startswith("about:") or len(value) <= 6:
                    return None
                if _ax_attr(address, kAXSelectedTextAttribute) != value[6:]:
                    return None
                return value

            completed = wait_for(completion)
            self.assertTrue(completed, f"completion text: {_ax_attr(address, kAXValueAttribute)!r}")
            return completed

        completed = type_internal_page_prefix()
        send_native_control_key(self.ctx.pid, 3, shift=True)  # F advances focus, preserving the suffix anchor.
        self.assertTrue(
            wait_for(lambda: _ax_attr(address, kAXSelectedTextAttribute) == completed[7:]),
            f"selection after Control-Shift-F: {_ax_attr(address, kAXSelectedTextAttribute)!r}",
        )
        self.assertEqual(_ax_attr(address, kAXValueAttribute), completed)
        send_native_control_key(self.ctx.pid, 0)
        send_native_control_key(self.ctx.pid, 40)
        self.assertTrue(wait_for(lambda: _ax_attr(address, kAXValueAttribute) == ""))
        previous = type_internal_page_prefix()
        send_native_key(self.ctx.pid, 125)  # Down explicitly chooses a suggestion.
        self.assertTrue(wait_for(lambda: _ax_attr(address, kAXValueAttribute) != previous))
        send_native_control_key(self.ctx.pid, 0)
        send_native_control_key(self.ctx.pid, 40)
        self.assertTrue(wait_for(lambda: _ax_attr(address, kAXValueAttribute) == ""))

    def test_address_field_custom_deletion(self):
        fields = find_all_by_role(self.ctx.window(), "AXTextField")
        address = next(
            (field for field in fields if str(_ax_attr(field, kAXValueAttribute) or "").startswith("file://")), None
        )
        self.assertIsNotNone(address, "address field missing")
        self.addCleanup(self._leave_address_field, address)
        send_native_key(self.ctx.pid, 37, kCGEventFlagMaskCommand)  # Command-L
        self.assertTrue(
            wait_for(lambda: bool(_ax_attr(address, kAXFocusedAttribute))),
            f"Command-L address focus: {_ax_attr(address, kAXFocusedAttribute)!r}",
        )
        self.assertEqual(AXUIElementSetAttributeValue(address, kAXValueAttribute, "hello world"), 0)
        send_native_control_key(self.ctx.pid, 14)
        send_native_control_key(self.ctx.pid, 13)
        expected = "hello " if self.MODE == "custom" else "hello world"
        self.assertTrue(
            wait_for(lambda: _ax_attr(address, kAXValueAttribute) == expected),
            f"address value: {_ax_attr(address, kAXValueAttribute)!r}",
        )
        send_native_control_key(self.ctx.pid, 0)
        send_native_control_key(self.ctx.pid, 40)
        self.assertTrue(
            wait_for(lambda: _ax_attr(address, kAXValueAttribute) == ""),
            f"address after Control-K: {_ax_attr(address, kAXValueAttribute)!r}",
        )
        send_native_key(self.ctx.pid, 6, kCGEventFlagMaskCommand)  # Command-Z
        self.assertTrue(
            wait_for(lambda: _ax_attr(address, kAXValueAttribute) == expected),
            f"address after undo: {_ax_attr(address, kAXValueAttribute)!r}",
        )


class NativeCustomControlKeyEditingTests(NativeControlKeyEditingTests):
    MODE = "custom"


if __name__ == "__main__":
    unittest.main()
