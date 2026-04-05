"""Custom script utilities for Ladybird browser.

Includes a fallback patch for AT-SPI2 Collection GetMatches, which
Qt's bridge exposes but returns empty results for. When Collection
fails, we fall back to a manual tree search. This is forward-
compatible: when Qt fixes Collection (expected in 6.11+), the
fallback patch is bypassed because Collection will return results
first."""

from __future__ import annotations

from orca import focus_manager
from orca.ax_object import AXObject
from orca.ax_utilities import AXUtilities
from orca.scripts import web


def _tree_search_by_role(root, roles, pred=None):
    """Manual DFS tree search for objects matching any of the given roles.

    This is the fallback when Qt's Collection GetMatches returns empty."""

    if root is None:
        return []

    results = []

    def walk(obj, depth=0):
        if obj is None or depth > 50:
            return
        role = AXObject.get_role(obj)
        if role in roles:
            if pred is None or pred(obj):
                results.append(obj)
        for i in range(AXObject.get_child_count(obj)):
            walk(AXObject.get_child(obj, i), depth + 1)

    walk(root)
    return results


def _tree_search_by_role_and_states(root, roles, states, pred=None):
    """Manual DFS tree search for objects matching any of the given roles
    AND having ALL of the given states."""

    if root is None:
        return []

    results = []

    def walk(obj, depth=0):
        if obj is None or depth > 50:
            return
        role = AXObject.get_role(obj)
        if role in roles:
            try:
                state_set = obj.get_state_set()
                has_all = all(state_set.contains(s) for s in states)
            except Exception:
                has_all = False
            if has_all and (pred is None or pred(obj)):
                results.append(obj)
        for i in range(AXObject.get_child_count(obj)):
            walk(AXObject.get_child(obj, i), depth + 1)

    walk(root)
    return results


_collection_fallback_installed = False
_sayall_patched = False


def _install_sayall_patch():
    """Monkey-patch SayAllPresenter.say_all to move locus to document
    content before starting, if locus is outside the document."""

    global _sayall_patched
    if _sayall_patched:
        return
    _sayall_patched = True

    try:
        from orca import say_all_presenter

        presenter = say_all_presenter.get_presenter()
        _orig_say_all = presenter.say_all

        def _patched_say_all(script, event=None, notify_user=True, obj=None, offset=None):
            if obj is None and hasattr(script, "utilities"):
                # Always clear cached document and caret context so
                # Say All finds the CURRENT tab's content, not stale
                # objects from a previous tab.
                script.utilities._cached_active_document = None
                script.utilities._cache_warmed = False
                script.utilities.clear_cached_objects()

                doc = script.utilities.active_document()
                if doc is not None:
                    first = (
                        script.utilities._find_first_content_child(doc)
                        if hasattr(script.utilities, "_find_first_content_child")
                        else None
                    )
                    if first is not None:
                        script.utilities.set_caret_context(first, 0, doc)
                        obj = first
                        offset = 0

            return _orig_say_all(script, event, notify_user, obj, offset)

        # Patch both the instance method AND the stored handler reference
        presenter.say_all = _patched_say_all
        if "sayAllHandler" in presenter._handlers:
            presenter._handlers["sayAllHandler"].function = _patched_say_all
    except Exception:
        pass


_scroll_patched = False


def _install_scroll_patch():
    """Monkey-patch AXEventSynthesizer.scroll_to_center to also trigger
    ScrollSubstringTo (which Qt handles) in addition to
    ScrollSubstringToPoint (which Qt does not handle). This makes the
    focus ring appear during structural navigation (H/K/I/L keys)."""

    global _scroll_patched
    if _scroll_patched:
        return
    _scroll_patched = True

    try:
        from orca.ax_event_synthesizer import AXEventSynthesizer
        from orca.ax_text import AXText

        _orig_scroll_to_center = AXEventSynthesizer.scroll_to_center

        @staticmethod
        def _patched_scroll_to_center(obj, start_offset=None, end_offset=None):
            # Call the original (tries ScrollSubstringToPoint + ScrollToPoint)
            _orig_scroll_to_center(obj, start_offset, end_offset)
            # Also try ScrollSubstringTo which Qt actually handles,
            # triggering our scrollToSubstring → focus action → focus ring
            try:
                length = AXText.get_character_count(obj)
                if length:
                    if start_offset is None:
                        start_offset = 0
                    if end_offset is None:
                        end_offset = length - 1
                    AXText.scroll_substring_to_location(obj, 0, start_offset, end_offset)
            except Exception:
                pass

        AXEventSynthesizer.scroll_to_center = _patched_scroll_to_center
    except Exception:
        pass


def _install_collection_fallback_patch():
    """Wrap AXUtilitiesCollection.find_all_with_role and
    find_all_with_role_and_all_states so they fall back to a manual
    DFS tree search when Qt's Collection GetMatches returns empty.

    Forward-compatible: when Qt fixes Collection, the fallback path is
    never taken because Collection returns results first."""

    global _collection_fallback_installed
    if _collection_fallback_installed:
        return
    _collection_fallback_installed = True

    try:
        from orca.ax_utilities_collection import AXUtilitiesCollection

        _original_find_all_with_role = AXUtilitiesCollection.find_all_with_role

        @staticmethod
        def _patched_find_all_with_role(root, role_list, pred=None):
            try:
                results = _original_find_all_with_role(root, role_list, pred)
                if results:
                    return results
            except Exception:
                pass
            return _tree_search_by_role(root, role_list, pred)

        AXUtilitiesCollection.find_all_with_role = _patched_find_all_with_role

        _original_find_all_with_role_and_all_states = AXUtilitiesCollection.find_all_with_role_and_all_states

        @staticmethod
        def _patched_find_all_with_role_and_all_states(root, role_list, state_list, pred=None):
            try:
                results = _original_find_all_with_role_and_all_states(root, role_list, state_list, pred)
                if results:
                    return results
            except Exception:
                pass
            return _tree_search_by_role_and_states(root, role_list, state_list, pred)

        AXUtilitiesCollection.find_all_with_role_and_all_states = _patched_find_all_with_role_and_all_states
    except Exception:
        pass


class Utilities(web.Utilities):
    def __init__(self, script):
        super().__init__(script)
        self._cached_active_document = None
        self._cache_warmed = False
        _install_collection_fallback_patch()
        _install_sayall_patch()
        _install_scroll_patch()

    def prewarm_cache(self):
        """Walk the document tree and query each object's key properties.

        This populates Orca's internal Python-side caches (role, name,
        states, attributes) so the first Say All doesn't need to make
        hundreds of cold D-Bus round-trips. Called from activate() via
        GLib.idle_add so it runs in the background without blocking."""

        if self._cache_warmed:
            return

        doc = self.active_document()
        if doc is None:
            return

        self._cache_warmed = True

        def walk(obj, depth=0):
            if obj is None or depth > 30:
                return
            try:
                AXObject.get_role(obj)
                AXObject.get_name(obj)
                AXObject.get_child_count(obj)
                obj.get_state_set()
                obj.get_attributes()
            except Exception:
                pass
            for i in range(AXObject.get_child_count(obj)):
                try:
                    walk(AXObject.get_child(obj, i), depth + 1)
                except Exception:
                    pass

        walk(doc)

        # Pre-compute the first caret context so Say All doesn't
        # need to call first_context() (which makes many D-Bus calls)
        first_obj, first_offset = self.first_context(doc, 0)
        if first_obj is not None:
            self.set_caret_context(first_obj, first_offset, doc)

    def active_document(self):
        """Returns the active document for the currently visible tab.

        Try super() first (uses EMBEDS relation, fast). Fall back to
        tree search if EMBEDS fails or returns a stale document."""

        # Try the standard EMBEDS-based approach first
        doc = super().active_document()
        if doc is not None:
            self._cached_active_document = doc
            return doc

        # Return cached document if still valid
        if self._cached_active_document is not None:
            if not AXObject.is_dead(self._cached_active_document) and AXUtilities.is_showing(
                self._cached_active_document
            ):
                return self._cached_active_document
            self._cached_active_document = None

        window = focus_manager.get_manager().get_active_window()
        if window is None:
            return None

        # Find ALL document_web nodes, prefer the one whose parent
        # panel is showing (active tab). Both documents report
        # showing=True, but the inactive tab's parent panel is hidden.
        documents = []

        def find_documents(obj, depth=0):
            if obj is None or depth > 15:
                return
            if AXUtilities.is_document_web(obj):
                documents.append(obj)
                return
            for i in range(AXObject.get_child_count(obj)):
                find_documents(AXObject.get_child(obj, i), depth + 1)

        find_documents(window)

        # Pick the document whose parent is showing (active tab)
        doc = None
        for d in documents:
            parent = AXObject.get_parent(d)
            if parent and AXUtilities.is_showing(parent):
                doc = d
                break
        # Fallback: if none matched, use the last one
        if doc is None and documents:
            doc = documents[-1]
        if doc is not None:
            self._cached_active_document = doc
        return doc

    def _get_text_for_obj(self, obj):
        """Get text content for an accessible object.

        Tries name first, then text interface, then recurses into
        children. This mirrors what our QAccessibleTextInterface's
        build_hypertext() does on the C++ side."""

        if obj is None:
            return ""

        # Text leaves and named elements: use the name
        name = AXObject.get_name(obj)
        if name:
            return name

        # Try text interface
        try:
            ti = obj.get_text_iface()
            if ti:
                cc = ti.get_character_count()
                if cc > 0:
                    return ti.get_text(0, cc)
        except Exception:
            pass

        # Container: collect text from children
        parts = []
        for i in range(AXObject.get_child_count(obj)):
            child = AXObject.get_child(obj, i)
            if child is not None:
                child_text = self._get_text_for_obj(child)
                if child_text:
                    parts.append(child_text)
        return " ".join(parts)

    def get_line_contents_at_offset(self, obj, offset, layout_mode=True, use_cache=True):
        """Build line contents with fewer D-Bus calls than the default.

        The default web script implementation scans left and right in
        layout mode, checking geometry for every neighboring object.
        This makes ~75-115 D-Bus round-trips per line. Our override
        gets the text directly from the accessible tree, avoiding the
        expensive geometry-based line scanning.

        Falls back to the default for non-document content."""

        import sys as _s2

        _in_doc = self.in_document_content(obj) if obj else False
        if not _in_doc:
            print("LADYBIRD: get_line_contents NOT in doc, falling through", file=_s2.stderr)
            return super().get_line_contents_at_offset(obj, offset, layout_mode, use_cache)

        try:
            text = self._get_text_for_obj(obj)
            if text:
                print("LADYBIRD: get_line_contents fast: '%s'" % text[:60], file=_s2.stderr)
                return [(obj, 0, len(text), text)]
            print("LADYBIRD: get_line_contents fast: empty text for obj", file=_s2.stderr)
        except Exception as _e:
            print("LADYBIRD: get_line_contents fast: exception %s" % _e, file=_s2.stderr)

        # Fall back to default if our approach didn't produce anything
        print("LADYBIRD: get_line_contents falling through to super", file=_s2.stderr)
        return super().get_line_contents_at_offset(obj, offset, layout_mode, use_cache)

    def get_caret_context(self, document=None, get_replicant=False, search_if_needed=True):
        """Returns the current caret context.

        If there's no existing context in document content, find the
        document and return its first content element so Say All starts
        from web content instead of the window title."""

        import sys as _sys

        obj, offset = super().get_caret_context(document, get_replicant, search_if_needed)

        if obj is not None:
            try:
                _r = AXObject.get_role(obj)
                _n = AXObject.get_name(obj) or ""
                _in = self.in_document_content(obj)
                print(
                    "LADYBIRD: get_caret_context super: role=%s name='%s' in_doc=%s offset=%s"
                    % (_r, _n[:40], _in, offset),
                    file=_sys.stderr,
                )
            except Exception:
                pass
        else:
            print("LADYBIRD: get_caret_context super returned None", file=_sys.stderr)

        if obj is not None and self.in_document_content(obj):
            # If the caret context is the document root itself (which has
            # no text), redirect to the first content child so Say All
            # reads web content instead of saying "Blank".
            if AXUtilities.is_document_web(obj):
                doc = obj
                first_obj, first_offset = self.first_context(doc, 0)
                if first_obj is not None:
                    self.set_caret_context(first_obj, first_offset, doc)
                    return first_obj, first_offset
            return obj, offset

        # Fall back: find the document and locate the first content element
        doc = self.active_document()
        if doc is not None:
            # Try first_context first (Orca's standard approach)
            first_obj, first_offset = self.first_context(doc, 0)
            # If first_context returned the document itself (meaning it
            # couldn't descend), manually find the first child with content
            if first_obj is None or first_obj == doc or AXUtilities.is_document_web(first_obj):
                first_obj = self._find_first_content_child(doc)
                first_offset = 0
            if first_obj is not None and not AXUtilities.is_document_web(first_obj):
                print(
                    "LADYBIRD: get_caret_context fallback: found role=%s name='%s'"
                    % (AXObject.get_role(first_obj), (AXObject.get_name(first_obj) or "")[:40]),
                    file=_sys.stderr,
                )
                self.set_caret_context(first_obj, first_offset, doc)
                return first_obj, first_offset
            print("LADYBIRD: get_caret_context fallback: using doc (no content child found)", file=_sys.stderr)
            self.set_caret_context(doc, 0, doc)
            return doc, 0
        else:
            print("LADYBIRD: get_caret_context fallback: no active_document found", file=_sys.stderr)

        return obj, offset

    def _find_first_content_child(self, obj, depth=0):
        """Find the first descendant that has text content."""
        if obj is None or depth > 15:
            return None
        for i in range(AXObject.get_child_count(obj)):
            child = AXObject.get_child(obj, i)
            if child is None:
                continue
            name = AXObject.get_name(child)
            if name:
                return child
            # Try text interface
            try:
                ti = child.get_text_iface()
                if ti and ti.get_character_count() > 0:
                    return child
            except Exception:
                pass
            # Recurse
            result = self._find_first_content_child(child, depth + 1)
            if result is not None:
                return result
        return None
