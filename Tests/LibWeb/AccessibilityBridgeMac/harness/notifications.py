"""AXObserver-based notification capture for the macOS accessibility-bridge tests."""

from __future__ import annotations

import time

from typing import List
from typing import Optional
from typing import Tuple

import objc

from ApplicationServices import AXObserverAddNotification
from ApplicationServices import AXObserverCreate
from ApplicationServices import AXObserverGetRunLoopSource
from ApplicationServices import AXObserverRemoveNotification
from CoreFoundation import CFRunLoopAddSource
from CoreFoundation import CFRunLoopGetCurrent
from CoreFoundation import CFRunLoopRemoveSource
from CoreFoundation import CFRunLoopRunInMode
from CoreFoundation import kCFRunLoopDefaultMode


class NotificationCollector:
    """Records the NSAccessibility notifications an application posts, as (notification name, element) pairs.

    AXObserver callbacks are delivered on the current thread's run loop, so nothing arrives until the caller pumps it:
    collect(seconds) runs the loop for that long and returns what came in. Register on the application element to see
    every notification the app posts, or on a specific element to see only that element's."""

    def __init__(self, pid: int, element, notifications: List[str]):
        self._element = element
        self._notifications = list(notifications)
        self.events: List[Tuple[str, object]] = []

        # PyObjC only passes a Python callable to a C function-pointer parameter once it's wrapped as a closure for
        # that exact function.
        @objc.callbackFor(AXObserverCreate)
        def _callback(_observer, elem, notification, _refcon):
            self.events.append((str(notification), elem))

        self._callback = _callback
        err, observer = AXObserverCreate(pid, _callback, None)
        if err != 0 or observer is None:
            raise RuntimeError(f"AXObserverCreate failed with error {err}")
        self._observer = observer
        for name in self._notifications:
            err = AXObserverAddNotification(self._observer, element, name, None)
            if err != 0:
                raise RuntimeError(f"AXObserverAddNotification({name!r}) failed with error {err}")
        self._source = AXObserverGetRunLoopSource(self._observer)
        CFRunLoopAddSource(CFRunLoopGetCurrent(), self._source, kCFRunLoopDefaultMode)

    def collect(self, seconds: float) -> List[Tuple[str, object]]:
        """Pumps the run loop for "seconds" and returns every event received so far."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, min(0.1, max(0.0, deadline - time.monotonic())), False)
        return list(self.events)

    def names(self) -> List[str]:
        return [name for name, _elem in self.events]

    def elements_for(self, notification: str) -> List[object]:
        return [elem for name, elem in self.events if name == notification]

    def clear(self) -> None:
        self.events.clear()

    def close(self) -> None:
        observer: Optional[object] = getattr(self, "_observer", None)
        if observer is None:
            return
        for name in self._notifications:
            try:
                AXObserverRemoveNotification(observer, self._element, name)
            except Exception:
                pass
        try:
            CFRunLoopRemoveSource(CFRunLoopGetCurrent(), self._source, kCFRunLoopDefaultMode)
        except Exception:
            pass
        self._observer = None
