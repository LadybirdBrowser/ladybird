"""unittest base class that launches Ladybird on a fixture page once per test case."""

from __future__ import annotations

import unittest

from .ladybird import FIXTURE_DIR
from .ladybird import LadybirdContext


class AccessibilityBridgeTestCase(unittest.TestCase):
    """Subclass this and set FIXTURE = "<name>.html" (a file under input/).

    Ladybird is launched once per TestCase subclass (setUpClass) and reused across every test method in that class. Each
    test method gets self.app (the Ladybird AT-SPI2 application) and self.doc (the document web accessible) already
    populated."""

    FIXTURE: str = ""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        if not cls.FIXTURE:
            raise unittest.SkipTest(f"{cls.__name__} didn't set FIXTURE")
        fixture_path = (FIXTURE_DIR / cls.FIXTURE).resolve()
        if not fixture_path.exists():
            raise unittest.SkipTest(f"fixture not found: {fixture_path}")
        cls._ctx = LadybirdContext(fixture_path.as_uri())
        cls._ctx.start()

    @classmethod
    def tearDownClass(cls):
        ctx = getattr(cls, "_ctx", None)
        if ctx is not None:
            ctx.stop()
        super().tearDownClass()

    @property
    def ctx(self) -> LadybirdContext:
        return type(self)._ctx

    @property
    def app(self):
        return self.ctx.app

    @property
    def doc(self):
        return self.ctx.document


class LadybirdOrcaTestCase(AccessibilityBridgeTestCase):
    """Layer-2 base class: on top of the Ladybird launch, opens an OrcaSession in setUp.

    The OrcaSession is per-test-method (not per-class) — so captured speech doesn't leak between tests. If Orca isn't
    installed, every test in the subclass is skipped automatically — with a clear message."""

    def setUp(self):
        super().setUp()
        from .orca import OrcaNotInstalled
        from .orca import OrcaSession

        try:
            self.orca = OrcaSession(self.app)
            self.orca.start()
        except OrcaNotInstalled as exc:
            raise unittest.SkipTest(str(exc)) from exc

    def tearDown(self):
        orca = getattr(self, "orca", None)
        if orca is not None:
            orca.stop()
        super().tearDown()
