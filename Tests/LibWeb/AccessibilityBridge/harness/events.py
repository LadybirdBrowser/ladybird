"""AT-SPI2 event capture for the bridge tests."""

from __future__ import annotations

import time

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402
from gi.repository import GLib  # noqa: E402


class EventCollector:
    """Collects the AT-SPI2 events of the given types, as (type, detail1, source role name, source name) tuples.

    Registering a listener is what makes Qt's adaptor emit the events at all: it sends focus and state-changed events
    only once some client has asked for them, and it learns of a new client through the registry — so pump() for a
    moment after constructing the collector, before triggering what it should see. Pumping the default GLib main
    context is also what delivers the events: libatspi dispatches them from that context, and the harness otherwise
    never runs it."""

    def __init__(self, *event_types: str):
        self.events: list[tuple[str, int, str | None, str | None]] = []
        self._event_types = event_types
        self._listener = Atspi.EventListener.new(self._on_event)
        for event_type in event_types:
            if not self._listener.register(event_type):
                raise RuntimeError(f"could not register an AT-SPI2 listener for {event_type!r}")

    def _on_event(self, event: Atspi.Event) -> None:
        role = name = None
        if event.source is not None:
            try:
                role = event.source.get_role_name()
                name = event.source.get_name()
            except GLib.GError:
                pass
        self.events.append((event.type, event.detail1, role, name))

    def pump(self, seconds: float) -> None:
        """Runs the default main context for the given time, so queued events reach the collector."""
        context = GLib.MainContext.default()
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            while context.iteration(False):
                pass
            time.sleep(0.02)

    def close(self) -> None:
        for event_type in self._event_types:
            self._listener.deregister(event_type)
