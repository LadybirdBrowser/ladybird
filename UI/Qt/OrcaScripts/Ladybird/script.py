"""Custom Orca script for Ladybird browser.

Extends the web script for document content, falling through to the
default script for browser chrome (address bar, tabs, menus) — the
same pattern Firefox/Gecko uses."""

from __future__ import annotations

from typing import TYPE_CHECKING

from orca import debug
from orca import focus_manager
from orca.ax_object import AXObject
from orca.ax_utilities import AXUtilities
from orca.scripts import default
from orca.scripts import web

from .script_utilities import Utilities

if TYPE_CHECKING:
    import gi
    gi.require_version("Atspi", "2.0")
    from gi.repository import Atspi


class Script(web.Script):

    def get_toolkit_name(self):
        return "Ladybird"

    def get_utilities(self) -> Utilities:
        return Utilities(self)

    def get_app_key_bindings(self):
        return web.Script.get_app_key_bindings(self)

    def activate(self):
        """Called when this script is activated (including at Orca startup).

        If the locus of focus is not in document content (e.g. Orca
        started after the page loaded), move it into the document so
        Say All and browse-mode navigation work immediately."""

        self._ensure_document_focus()
        super().activate()

    def _ensure_document_focus(self):
        """If locus of focus is not in document content, move it there."""

        focus = focus_manager.get_manager().get_locus_of_focus()
        if focus and self.utilities.in_document_content(focus):
            return

        doc = self.utilities.active_document()
        if doc is None:
            return

        first_obj, first_offset = self.utilities.first_context(doc, 0)
        if first_obj is None:
            first_obj = doc
            first_offset = 0

        self.utilities.set_caret_context(first_obj, first_offset, doc)
        focus_manager.get_manager().set_locus_of_focus(None, first_obj, notify_script=False)

    def locus_of_focus_changed(self, event, old_focus, new_focus):
        if super().locus_of_focus_changed(event, old_focus, new_focus):
            return True
        return default.Script.locus_of_focus_changed(self, event, old_focus, new_focus)

    def on_focused_changed(self, event):
        if super().on_focused_changed(event):
            return True
        result = default.Script.on_focused_changed(self, event)
        self._ensure_document_focus()
        return result

    def on_window_activated(self, event):
        result = False
        if super().on_window_activated(event):
            result = True
        else:
            result = default.Script.on_window_activated(self, event)
        self._ensure_document_focus()
        return result

    def on_caret_moved(self, event):
        if super().on_caret_moved(event):
            return True
        return default.Script.on_caret_moved(self, event)

    def on_active_changed(self, event):
        if super().on_active_changed(event):
            return True
        return default.Script.on_active_changed(self, event)

    def on_children_added(self, event):
        if super().on_children_added(event):
            return True
        return default.Script.on_children_added(self, event)

    def on_children_removed(self, event):
        if super().on_children_removed(event):
            return True
        return default.Script.on_children_removed(self, event)

    def on_document_load_complete(self, event):
        if super().on_document_load_complete(event):
            return True
        return default.Script.on_document_load_complete(self, event)

    def on_name_changed(self, event):
        if super().on_name_changed(event):
            return True
        return default.Script.on_name_changed(self, event)

    def on_selected_changed(self, event):
        if super().on_selected_changed(event):
            return True
        return default.Script.on_selected_changed(self, event)

    def on_selection_changed(self, event):
        if super().on_selection_changed(event):
            return True
        return default.Script.on_selection_changed(self, event)

    def on_showing_changed(self, event):
        if super().on_showing_changed(event):
            return True
        return default.Script.on_showing_changed(self, event)

    def on_text_deleted(self, event):
        if super().on_text_deleted(event):
            return True
        return default.Script.on_text_deleted(self, event)

    def on_text_inserted(self, event):
        if super().on_text_inserted(event):
            return True
        return default.Script.on_text_inserted(self, event)

    def on_text_selection_changed(self, event):
        if super().on_text_selection_changed(event):
            return True
        return default.Script.on_text_selection_changed(self, event)

    def on_window_deactivated(self, event):
        if super().on_window_deactivated(event):
            return True
        return default.Script.on_window_deactivated(self, event)
