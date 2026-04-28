"""Unit tests for Utilities.active_document()'s cache revalidation.

These need Orca importable (the script subclasses orca.scripts.web.Utilities) but neither a running Ladybird nor an Orca
session: they call the method on a bare Utilities instance with the AX helpers it consults replaced by fakes."""

from __future__ import annotations

import pathlib
import sys
import unittest

from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from harness.orca import OrcaNotInstalled  # noqa: E402
from harness.orca import load_ladybird_script_utilities_module  # noqa: E402


class _FakeAXObject:
    """AXObject stand-in: liveness and parentage come from the test's own tables."""

    dead = set()
    parents = {}

    @classmethod
    def is_dead(cls, obj):
        return obj in cls.dead

    @classmethod
    def get_parent(cls, obj):
        return cls.parents.get(obj)


class _FakeAXUtilities:
    showing = set()

    @classmethod
    def is_showing(cls, obj):
        return obj in cls.showing


class ActiveDocumentCacheRevalidationTests(unittest.TestCase):
    """active_document() keeps the last document it resolved and hands it back while it still looks live. After a tab
    switch that document belongs to a background tab, and its own state can't tell: it still reports showing=True, and
    only its parent panel goes hidden. So the cache has to be revalidated through the parent, exactly like a fresh
    EMBEDS answer is; without that, Orca kept navigating the old tab's content after a switch."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        try:
            cls.module = load_ladybird_script_utilities_module()
        except OrcaNotInstalled as exc:
            raise unittest.SkipTest(str(exc)) from exc

    def setUp(self):
        _FakeAXObject.dead = set()
        _FakeAXObject.parents = {}
        _FakeAXUtilities.showing = set()
        # No active window: the tree search after a cache miss finds nothing, so a document can only come from the
        # cache or from the EMBEDS answer, which is what each test controls.
        no_window = SimpleNamespace(get_manager=lambda: SimpleNamespace(get_active_window=lambda: None))
        patches = [
            mock.patch.object(self.module, "AXObject", _FakeAXObject),
            mock.patch.object(self.module, "AXUtilities", _FakeAXUtilities),
            mock.patch.object(self.module, "focus_manager", no_window),
            mock.patch.object(self.module.web.Utilities, "active_document", lambda self: None),
        ]
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)

    def _utilities_with_cached(self, cached):
        # Bypass __init__: it needs a live script and installs process-wide Orca patches; the cache field is all that
        # active_document() reads on the instance.
        utils = self.module.Utilities.__new__(self.module.Utilities)
        utils._cached_active_document = cached
        return utils

    def test_cached_document_whose_parent_is_showing_is_reused(self):
        doc, parent = object(), object()
        _FakeAXObject.parents[doc] = parent
        _FakeAXUtilities.showing.update({doc, parent})
        utils = self._utilities_with_cached(doc)
        self.assertIs(utils.active_document(), doc)
        self.assertIs(utils._cached_active_document, doc)

    def test_cached_document_whose_parent_is_hidden_is_dropped(self):
        """The tab-switch case: the document itself still reports showing, its parent panel doesn't."""
        doc, parent = object(), object()
        _FakeAXObject.parents[doc] = parent
        _FakeAXUtilities.showing.add(doc)
        utils = self._utilities_with_cached(doc)
        self.assertIsNone(utils.active_document(), "a background tab's document came back as the active one")
        self.assertIsNone(utils._cached_active_document, "the stale document is still cached")

    def test_dead_cached_document_is_dropped(self):
        doc = object()
        _FakeAXObject.dead.add(doc)
        _FakeAXUtilities.showing.add(doc)
        utils = self._utilities_with_cached(doc)
        self.assertIsNone(utils.active_document())
        self.assertIsNone(utils._cached_active_document)

    def test_cached_document_without_a_parent_is_dropped(self):
        doc = object()
        _FakeAXUtilities.showing.add(doc)
        utils = self._utilities_with_cached(doc)
        self.assertIsNone(utils.active_document())
        self.assertIsNone(utils._cached_active_document)

    def test_embeds_answer_with_a_showing_parent_replaces_the_cache(self):
        old_doc, new_doc, parent = object(), object(), object()
        _FakeAXObject.parents[new_doc] = parent
        _FakeAXUtilities.showing.update({new_doc, parent})
        with mock.patch.object(self.module.web.Utilities, "active_document", lambda self: new_doc):
            utils = self._utilities_with_cached(old_doc)
            self.assertIs(utils.active_document(), new_doc)
            self.assertIs(utils._cached_active_document, new_doc)


if __name__ == "__main__":
    unittest.main()
