"""Custom Orca script for Ladybird browser."""

from __future__ import annotations

from orca.ax_utilities import AXUtilities
from orca.scripts import default
from orca.scripts import web

from .script_utilities import Utilities

try:
    from orca.scripts.toolkits.Qt.script import Script as _QtToolkitScript
except ImportError:
    _QtToolkitScript = None

if _QtToolkitScript is not None:

    class Script(web.Script, _QtToolkitScript):
        pass
else:

    class Script(web.Script):
        pass


class Script(Script):
    def get_toolkit_name(self):
        return "Ladybird"

    def get_utilities(self) -> Utilities:
        return Utilities(self)

    def get_app_key_bindings(self):
        return web.Script.get_app_key_bindings(self)

    def activate(self):
        """Called when this script is activated."""
        super().activate()
        # Pre-warm Orca's D-Bus caches in the background
        from gi.repository import GLib

        GLib.idle_add(self.utilities.prewarm_cache)

    # NOTE: on_focused_changed falls through to default.Script so Orca
    # properly handles chrome elements (address bar, tabs). Without
    # this, typing in the address bar breaks because Orca doesn't
    # enter focus mode for text entries.
    #
    # NOTE: on_window_activated must NOT be overridden. Falling through
    # to default.Script.on_window_activated sets locus to the window
    # frame, which causes the web script to suspend structural nav
    # permanently on fresh Ladybird start.

    def on_focused_changed(self, event):
        if super().on_focused_changed(event):
            return True
        if AXUtilities.is_entry(event.source) or AXUtilities.is_editable(event.source):
            # Explicitly suspend structural nav for text entries so
            # keys like H/I/K pass through for typing. The web script's
            # own suspension logic may skip this when both old and new
            # focus are outside document content.
            reason = "Ladybird: focus on editable chrome widget"
            self.get_structural_navigator().suspend_commands(self, True, reason)
            self.get_caret_navigator().suspend_commands(self, True, reason)
            return default.Script.on_focused_changed(self, event)
        return False
