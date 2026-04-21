"""Custom Orca script for Ladybird browser."""

from __future__ import annotations

from orca import document_presenter
from orca import structural_navigator
from orca.ax_utilities import AXUtilities
from orca.scripts import default
from orca.scripts import web
from orca.structural_navigator import NavigationMode

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
        # Put the structural navigator into DOCUMENT mode at script activation. web.Script only sets this when
        # on_focused_changed reaches its tail (focus transitioning from outside the document to a focusable element
        # inside it) — which doesn't fire for Ladybird's document-root focus event (web.Script short-circuits on
        # source == document). Without this, H/K/L bindings stay unregistered until you navigate into a focusable
        # in-page element; the suspension state still controls whether the bindings fire as commands — so URL-bar
        # typing is unaffected.
        super().activate()
        structural_navigator.get_navigator().set_mode(self, NavigationMode.DOCUMENT)

    # _on_focused_changed delegates to default.Script for any event web.Script didn't handle — so Orca still announces
    # focus changes on chrome widgets (back/forward buttons, tab strip), and enters focus mode for text entries like the
    # address bar. Without the delegation, tabbing through chrome is silent.
    #
    # The leading underscore in the method name is required: Orca 50+ dispatches "object:state-changed:focused" events
    # to self._on_focused_changed. The pre-50 spelling "on_focused_changed" (no underscore) is dead code under Orca 50 —
    # the dispatcher never invokes it, so any override using the old name is silently ignored.
    def _on_focused_changed(self, event):
        # Ladybird posts a Focus event on the document root whenever WebContentView gains focus (e.g., from tabbing into
        # the web content) and after each page load. web.Script._on_focused_changed short-circuits on source ==
        # document, which skips its tail set_mode(DOCUMENT) + un-suspend logic. Thus, we do that tail work ourselves
        # whenever focus lands anywhere in document content — so H/K/L become active without requiring the user to tab
        # to a focusable in-page element first.
        #
        # We use document_presenter.suspend_navigators() rather than driving the individual navigators ourselves —
        # which is the public API in Orca 50+. It's also the only public surface that does the un-suspend on both
        # structural_navigator and caret_navigator at once. table_navigator and live_region_presenter exist but have
        # *no* suspend_commands method in Orca 50+ — so the table/live-region branches we previously had against pre-50
        # APIs are dropped entirely.
        if self.utilities.in_document_content(event.source):
            reason = "Ladybird: focus in document content"
            structural_navigator.get_navigator().set_mode(self, NavigationMode.DOCUMENT)
            document_presenter.get_presenter().suspend_navigators(self, False, reason)
        if super()._on_focused_changed(event):
            return True
        if (AXUtilities.is_entry(event.source) or AXUtilities.is_editable(event.source)) and event.detail1:
            # Suspend browse-mode commands — so keys like H/I/K pass through to the text entry for typing.
            reason = "Ladybird: focus on editable chrome widget"
            document_presenter.get_presenter().suspend_navigators(self, True, reason)
        return default.Script._on_focused_changed(self, event)
