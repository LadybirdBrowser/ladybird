"""unittest base class that launches Ladybird on a fixture page once per test case."""

from __future__ import annotations

import unittest

from .ladybird import FIXTURE_DIR
from .ladybird import LadybirdContext


class AccessibilityBridgeMacTestCase(unittest.TestCase):
    """Subclass this and set FIXTURE = "<name>.html" (a file under AccessibilityBridge/input/).

    Ladybird is launched once per TestCase subclass (setUpClass) and reused across every test method in that class.
    Each test method gets self.app (the Ladybird app AXUIElement) and self.web (the AXWebArea) already populated."""

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
    def web(self):
        return self.ctx.web
