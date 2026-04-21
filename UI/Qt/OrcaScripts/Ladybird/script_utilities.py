"""Orca custom-script utilities for Ladybird."""

from __future__ import annotations

from orca import focus_manager
from orca.ax_object import AXObject
from orca.ax_utilities import AXUtilities
from orca.scripts import web


def _tree_search_by_role(root, roles, pred=None):
    """DFS tree search for objects matching any of the given roles — when Qt's Collection GetMatches returns empty."""

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
    """DFS tree search for objects matching any of the given roles — *and* having *all* the given states."""

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
_scroll_patched = False
_sayall_patched = False
_flat_review_dedup_patched = False


def _install_scroll_patch():
    """Monkey-patch AXEventSynthesizer.scroll_to_center to also trigger ScrollSubstringTo.

    Orca's default scroll_to_center uses ScrollSubstringToPoint, followed by ScrollToPoint (neither of which Qt's bridge
    implements). Neither reaches us, so neither the scroll nor the focus ring appears during browse-mode structural nav.

    Qt *does* route ScrollSubstringToLocation to our scrollToSubstring - which we wire to a 'scroll_into_view'
    accessibility action. That action scrolls the element and marks it as the document's accessibility focus target, so
    :focus-visible paints an outline — without moving DOM focus, so no focus event fires back to Orca."""

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
            _orig_scroll_to_center(obj, start_offset, end_offset)
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


def _install_sayall_patch():
    """Monkey-patch SayAllPresenter.say_all to redirect Say All to the document’s first content child — when locus of
    focus is not in web content (e.g., focus is on the address bar after the user types a URL and presses Enter).

    Orca’s say_all uses focus_manager.get_locus_of_focus() to choose the starting object; it does *not* route through
    get_caret_context — so our get_caret_context override doesn't help here. Without this patch, a Say All right after a
    navigation unexpectedly reads the currently-focused chrome widget — rather than reading the expected web content."""

    global _sayall_patched
    if _sayall_patched:
        return
    _sayall_patched = True

    try:
        from orca import focus_manager
        from orca import say_all_presenter

        presenter = say_all_presenter.get_presenter()
        _orig_say_all = presenter.say_all

        def _patched_say_all(script, event=None, notify_user=True, obj=None, offset=None):
            if obj is None and hasattr(script, "utilities"):
                candidate = focus_manager.get_manager().get_locus_of_focus()
                if candidate is None or not script.utilities.in_document_content(candidate):
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

        presenter.say_all = _patched_say_all
        if hasattr(presenter, "_handlers") and "sayAllHandler" in presenter._handlers:
            presenter._handlers["sayAllHandler"].function = _patched_say_all
    except Exception:
        pass


def _install_collection_fallback_patch():
    """Wrap AXUtilitiesCollection.find_all_with_role and find_all_with_role_and_all_states so they fall back to a DFS
    tree search when Qt's Collection GetMatches returns empty."""

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


def _install_flat_review_dedup_patch():
    """Monkey-patch AXUtilities._get_on_screen_objects to hide descendant objects whose text is already fully included
    in an ancestor’s flattened text.

    Context: a list item like <li>See <a>here</a> for details</li> exposes to AT-SPI2 a listitem with text “See here for
    details” (flattened via build_hypertext) *and* a link child with text "here". Orca’s flat review creates a Zone per
    on-screen object – so, without this patch, the user hears "See here for details" from the listitem zone, and then
    *also* "here" *again*, from the link zone — duplicated. So, this patch filters the descendant zone when its full
    text is a substring of an ancestor’s text that is also in the object list.

    This fix is only needed for flat review – and only affects flat review. Collection-based structural nav and the DOM
    focus chain don't go through _get_on_screen_objects — so they're unaffected by this."""

    global _flat_review_dedup_patched
    if _flat_review_dedup_patched:
        return
    _flat_review_dedup_patched = True

    try:
        import gi

        gi.require_version("Atspi", "2.0")
        from gi.repository import Atspi
        from orca.ax_object import AXObject
        from orca.ax_text import AXText
        from orca.ax_utilities import AXUtilities

        _orig = AXUtilities._get_on_screen_objects

        @staticmethod
        def _patched_get_on_screen_objects(root, cancellation_event, bounding_box=None):
            objects = _orig(root, cancellation_event, bounding_box)
            if not objects:
                return objects

            obj_set = set(objects)
            filtered = []

            def _text_of(obj):
                try:
                    return (AXText.get_all_text(obj) or "").strip()
                except Exception:
                    return ""

            # Only dedup against ancestors that flatten descendant text into their own Atspi.Text: the leaf-like
            # container roles in AccessibilityInterface.cpp (listitem, button, heading, link, etc.). For non-leaf-like
            # containers (paragraph, section, etc.), build_hypertext emits U+FFFC for embedded objects rather than their
            # text. Thus, a descendant's text can never be a substring of the ancestor's text, and dedup must not fire —
            # otherwise links inside paragraphs end up filtered out of flat review on pages whose link text happens to
            # coincide with surrounding text.
            _flattening_roles = {
                Atspi.Role.LIST_ITEM,
                Atspi.Role.PUSH_BUTTON,
                Atspi.Role.TOGGLE_BUTTON,
                Atspi.Role.HEADING,
                Atspi.Role.LINK,
                Atspi.Role.IMAGE,
                Atspi.Role.MENU_ITEM,
                Atspi.Role.PAGE_TAB,
                Atspi.Role.CHECK_BOX,
                Atspi.Role.RADIO_BUTTON,
            }
            for obj in objects:
                obj_text = _text_of(obj)
                skip = False
                if obj_text:
                    ancestor = AXObject.get_parent(obj)
                    depth = 0
                    while ancestor is not None and depth < 8:
                        if ancestor in obj_set and AXObject.get_role(ancestor) in _flattening_roles:
                            anc_text = _text_of(ancestor)
                            if anc_text and obj_text in anc_text:
                                skip = True
                                break
                        ancestor = AXObject.get_parent(ancestor)
                        depth += 1
                if not skip:
                    filtered.append(obj)

            return filtered

        AXUtilities._get_on_screen_objects = _patched_get_on_screen_objects
    except Exception:
        pass


_flat_review_document_scope_patched = False


def _install_flat_review_document_scope_patch():
    """Monkey-patch flat_review.Context._get_showing_zones so its cliprect covers the entire web document (not just the
    visible viewport). By default, Orca uses the toolkit window's rect as the cliprect — which clips away any
    paragraphs/links/etc. that are scrolled off the bottom of the viewport. Orca+L in flat review then stops at the
    last on-screen zone. For web content, we want flat review to walk through the whole document; visual scrolling is
    handled separately by _install_flat_review_scroll_patch, so the visible area follows the review position.

    When the top-level root passed to _get_showing_zones (or its descendants we can find quickly) is a web document,
    expand the bounding box to the document's own rect before delegating to the original implementation. Non-web
    contexts (menus, dialogs, chrome widgets) fall through unchanged."""
    global _flat_review_document_scope_patched
    if _flat_review_document_scope_patched:
        return
    _flat_review_document_scope_patched = True

    try:
        import gi

        gi.require_version("Atspi", "2.0")
        from gi.repository import Atspi
        from orca import flat_review
        from orca.ax_component import AXComponent
        from orca.ax_object import AXObject
        from orca.ax_utilities import AXUtilities

        _orig_get_showing_zones = flat_review.Context._get_showing_zones

        def _find_document_web(root):
            if root is None:
                return None
            if AXUtilities.is_document_web(root):
                return root
            return AXObject.find_descendant(root, AXUtilities.is_document_web)

        def _patched_get_showing_zones(self, root, boundingbox=None):
            if boundingbox is None:
                boundingbox = self._rect
            doc = _find_document_web(root)
            if doc is None:
                doc = _find_document_web(self._container)
            if doc is None:
                doc = _find_document_web(self._top_level)
            if doc is not None:
                try:
                    doc_rect = AXComponent.get_rect(doc)
                    if doc_rect.width > 0 and doc_rect.height > 0:
                        expanded = Atspi.Rect()
                        expanded.x = min(boundingbox.x, doc_rect.x)
                        expanded.y = min(boundingbox.y, doc_rect.y)
                        right = max(boundingbox.x + boundingbox.width, doc_rect.x + doc_rect.width)
                        bottom = max(boundingbox.y + boundingbox.height, doc_rect.y + doc_rect.height)
                        expanded.width = right - expanded.x
                        expanded.height = bottom - expanded.y
                        boundingbox = expanded
                except Exception:
                    pass
            return _orig_get_showing_zones(self, root, boundingbox)

        flat_review.Context._get_showing_zones = _patched_get_showing_zones
    except Exception:
        pass


_flat_review_scroll_patched = False


def _install_flat_review_scroll_patch():
    """Auto-scroll the page to follow the flat-review cursor. When you advance flat review (Orca+L for next item,
    Orca+KP_8/2 for line up/down, etc.), the visible area should track what's being read — matching Say All behavior.

    Wraps Context.go_next_word/go_next_line/go_next_character (and their previous/home/end counterparts) — so each
    successful advance triggers scroll_substring_to_location on the new current zone's object. Wrapping at the Context
    level rather than Presenter level catches every path that advances the cursor, including braille-driven autoscroll
    inside present_line, and runs before the speech generator — so the page scrolls as soon as the cursor moves."""
    global _flat_review_scroll_patched
    if _flat_review_scroll_patched:
        return
    _flat_review_scroll_patched = True

    try:
        from orca import flat_review
        from orca.ax_event_synthesizer import AXEventSynthesizer

        context_cls = flat_review.Context

        def _scroll_current_zone(context):
            if context is None:
                return
            try:
                zone = context._get_current_zone()
                if zone is None:
                    return
                obj = zone.get_object() if hasattr(zone, "get_object") else None
                if obj is None:
                    return
                start = zone.get_start_offset() if hasattr(zone, "get_start_offset") else 0
                end = start + len(zone.get_string()) if hasattr(zone, "get_string") else start + 1
                AXEventSynthesizer.scroll_into_view(obj, start, end)
            except Exception:
                pass

        def _wrap(method_name):
            orig = getattr(context_cls, method_name, None)
            if orig is None:
                return

            def wrapper(self, *args, **kwargs):
                advanced = orig(self, *args, **kwargs)
                if advanced:
                    _scroll_current_zone(self)
                return advanced

            setattr(context_cls, method_name, wrapper)

        for name in (
            "go_next_word",
            "go_previous_word",
            "go_next_line",
            "go_previous_line",
            "go_next_character",
            "go_previous_character",
            "go_next_zone",
            "go_previous_zone",
            "go_up",
            "go_down",
            "go_to_start_of",
            "go_to_end_of",
        ):
            _wrap(name)
    except Exception:
        pass


_hypertext_fallback_patched = False


def _install_hypertext_fallback_patch():
    """Monkey-patch AXHypertext helpers that Qt's bridge can't satisfy (no AtkHypertext/AtkHyperlink) — so Say All walks
    links correctly on Ladybird web content. Specifically:

    - find_child_at_offset: when called at a U+FFFC position inside a paragraph, return the embedded-object child whose
      U+FFFC that is. Otherwise, without this, find_next_caret_in_order can't descend into links at all — and Say All
      reads text around links but silently drops the link text itself.

    - get_link_start_offset/get_link_end_offset/get_character_offset_in_parent: return the U+FFFC position of the child
      in its parent's text. Without these, _find_next_caret_in_order_internal, on exiting a link, fails the
      range-in-parent check and falls back to AXObject.get_next_sibling — which (because collect_exposed_children hides
      text-leaf children) jumps straight to the next link and skips all the post-link prose. With these, Orca correctly
      returns to the parent paragraph at offset U+FFFC+1 and continues reading the text after the link.

    Our collect_exposed_children hides text-leaf children from AT-SPI2 — so a paragraph's exposed children are exactly
    the embedded-object descendants in DOM order. That means the Nth U+FFFC marker in the paragraph's Atspi.Text
    corresponds to paragraph.children[N] — which is what lets us compute both the descent (offset → child) and the
    ascent (child → offset) purely from parent text + exposed child list."""
    global _hypertext_fallback_patched
    if _hypertext_fallback_patched:
        return
    _hypertext_fallback_patched = True

    try:
        from orca.ax_hypertext import AXHypertext
        from orca.ax_object import AXObject
        from orca.ax_text import AXText

        def _child_fffc_offset(obj):
            """Return the U+FFFC position of obj inside its parent's text, or -1."""
            if obj is None:
                return -1
            parent = AXObject.get_parent(obj)
            if parent is None:
                return -1
            n = AXObject.get_child_count(parent)
            child_idx = -1
            for i in range(n):
                if AXObject.get_child(parent, i) == obj:
                    child_idx = i
                    break
            if child_idx < 0:
                return -1
            text = AXText.get_all_text(parent)
            if not text:
                return -1
            count = 0
            for i, c in enumerate(text):
                if c == "\ufffc":
                    if count == child_idx:
                        return i
                    count += 1
            return -1

        _orig_find_child = AXHypertext.find_child_at_offset

        @staticmethod
        def _patched_find_child(obj, offset):
            child = _orig_find_child(obj, offset)
            if child is not None:
                return child
            if obj is None or offset < 0:
                return None
            try:
                count = AXText.get_character_count(obj)
                if offset >= count:
                    return None
                text = AXText.get_all_text(obj)
                if not text or offset >= len(text) or text[offset] != "\ufffc":
                    return None
                fffc_index = text[:offset].count("\ufffc")
                child_count = AXObject.get_child_count(obj)
                if 0 <= fffc_index < child_count:
                    return AXObject.get_child(obj, fffc_index)
            except Exception:
                pass
            return None

        _orig_link_start = AXHypertext.get_link_start_offset
        _orig_link_end = AXHypertext.get_link_end_offset
        _orig_char_offset = AXHypertext.get_character_offset_in_parent

        @staticmethod
        def _patched_link_start(obj):
            result = _orig_link_start(obj)
            if result >= 0:
                return result
            try:
                return _child_fffc_offset(obj)
            except Exception:
                return -1

        @staticmethod
        def _patched_link_end(obj):
            result = _orig_link_end(obj)
            if result >= 0:
                return result
            try:
                offset = _child_fffc_offset(obj)
                return offset + 1 if offset >= 0 else -1
            except Exception:
                return -1

        @staticmethod
        def _patched_char_offset(obj):
            result = _orig_char_offset(obj)
            if result >= 0:
                return result
            try:
                return _child_fffc_offset(obj)
            except Exception:
                return -1

        AXHypertext.find_child_at_offset = _patched_find_child
        AXHypertext.get_link_start_offset = _patched_link_start
        AXHypertext.get_link_end_offset = _patched_link_end
        AXHypertext.get_character_offset_in_parent = _patched_char_offset
    except Exception:
        pass


class Utilities(web.Utilities):
    def __init__(self, script):
        super().__init__(script)
        self._cached_active_document = None
        _install_collection_fallback_patch()
        _install_scroll_patch()
        _install_sayall_patch()
        _install_flat_review_dedup_patch()
        _install_flat_review_document_scope_patch()
        _install_flat_review_scroll_patch()
        _install_hypertext_fallback_patch()

    def active_document(self):
        """Returns the active document for the currently-visible tab.

        Tries super() first (fast because it uses the EMBEDS relation) — and if EMBEDS fails or returns a stale
        document, then falls back to tree search."""

        # Try the standard EMBEDS-based approach first
        doc = super().active_document()
        if doc is not None:
            self._cached_active_document = doc
            return doc

        if self._cached_active_document is not None:
            if not AXObject.is_dead(self._cached_active_document) and AXUtilities.is_showing(
                self._cached_active_document
            ):
                return self._cached_active_document
            self._cached_active_document = None

        window = focus_manager.get_manager().get_active_window()
        if window is None:
            return None

        # Find *all* document_web nodes — preferring the one whose parent panel is showing (active tab). Both documents
        # report showing=True — but the inactive tab's parent panel is hidden.
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

        # Pick the document whose parent is showing (active tab).
        doc = None
        for d in documents:
            parent = AXObject.get_parent(d)
            if parent and AXUtilities.is_showing(parent):
                doc = d
                break
        # Fallback: if none matched, use the last one.
        if doc is None and documents:
            doc = documents[-1]
        if doc is not None:
            self._cached_active_document = doc
        return doc

    def get_caret_context(self, document=None, get_replicant=False, search_if_needed=True):
        """Returns the current caret context.

        If there's no existing context in document content, find the document and return its first content element – so
        Say All starts from the web content, rather than from the window title."""

        obj, offset = super().get_caret_context(document, get_replicant, search_if_needed)

        if obj is not None and self.in_document_content(obj):
            # If the caret context is the document root itself (which has no text), redirect to the first content child.
            if AXUtilities.is_document_web(obj):
                doc = obj
                first_obj, first_offset = self.first_context(doc, 0)
                if first_obj is not None:
                    self.set_caret_context(first_obj, first_offset, doc)
                    return first_obj, first_offset
            return obj, offset

        # Fallback: find the document and locate the first content element.
        doc = self.active_document()
        if doc is not None:
            # Try first_context first (Orca's standard approach).
            first_obj, first_offset = self.first_context(doc, 0)
            # If first_context returned the document itself (meaning it couldn't descend), then manually find the first
            # child with content.
            if first_obj is None or first_obj == doc or AXUtilities.is_document_web(first_obj):
                first_obj = self._find_first_content_child(doc)
                first_offset = 0
            if first_obj is not None and not AXUtilities.is_document_web(first_obj):
                self.set_caret_context(first_obj, first_offset, doc)
                return first_obj, first_offset
            self.set_caret_context(doc, 0, doc)
            return doc, 0

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
            try:
                ti = child.get_text_iface()
                if ti and ti.get_character_count() > 0:
                    return child
            except Exception:
                pass
            result = self._find_first_content_child(child, depth + 1)
            if result is not None:
                return result
        return None
