"""Accessibility-bridge test harness (macOS): launch Ladybird + drive it via NSAccessibility."""

from .ladybird import DEFAULT_BINARY
from .ladybird import FIXTURE_DIR
from .ladybird import LadybirdContext
from .ladybird import ladybird_for_fixture
from .ladybird import wait_for
from .testcase import AccessibilityBridgeMacTestCase
from .tree import dump_subtree
from .tree import find_all_by_role
from .tree import find_first_by_role
from .tree import get_attribute_names
from .tree import get_parameterized_attribute_names
from .tree import get_parameterized_attribute_value
from .tree import role_path
from .tree import supports_attribute
from .tree import wait_for_descendant_by_role
from .tree import walk

__all__ = [
    "AccessibilityBridgeMacTestCase",
    "DEFAULT_BINARY",
    "FIXTURE_DIR",
    "LadybirdContext",
    "dump_subtree",
    "find_all_by_role",
    "find_first_by_role",
    "get_attribute_names",
    "get_parameterized_attribute_names",
    "get_parameterized_attribute_value",
    "ladybird_for_fixture",
    "role_path",
    "supports_attribute",
    "wait_for",
    "wait_for_descendant_by_role",
    "walk",
]
