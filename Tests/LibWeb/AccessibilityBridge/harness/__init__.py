"""Accessibility-bridge test harness: launch Ladybird + drive it over AT-SPI2."""

from .ladybird import DEFAULT_BINARY
from .ladybird import FIXTURE_DIR
from .ladybird import LadybirdContext
from .ladybird import ladybird_for_fixture
from .ladybird import wait_for
from .orca import OrcaNotInstalled
from .orca import OrcaSession
from .testcase import AccessibilityBridgeTestCase
from .testcase import LadybirdOrcaTestCase
from .tree import dump_subtree
from .tree import find_all_by_role
from .tree import find_first_by_role
from .tree import get_attributes_dict
from .tree import role_path
from .tree import supports_interface
from .tree import wait_for_descendant_by_role
from .tree import walk

__all__ = [
    "LadybirdContext",
    "ladybird_for_fixture",
    "wait_for",
    "FIXTURE_DIR",
    "DEFAULT_BINARY",
    "AccessibilityBridgeTestCase",
    "LadybirdOrcaTestCase",
    "OrcaNotInstalled",
    "OrcaSession",
    "find_first_by_role",
    "find_all_by_role",
    "wait_for_descendant_by_role",
    "walk",
    "role_path",
    "dump_subtree",
    "get_attributes_dict",
    "supports_interface",
]
