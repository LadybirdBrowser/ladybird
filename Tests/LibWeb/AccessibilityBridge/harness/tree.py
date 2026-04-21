"""AT-SPI2 tree walking and query helpers."""

from __future__ import annotations

from typing import Callable
from typing import List
from typing import Optional

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402


def walk(
    root: Atspi.Accessible,
    visit: Callable[[Atspi.Accessible, int], None],
    *,
    max_depth: int = 30,
) -> None:
    """Pre-order DFS of the AT-SPI2 subtree rooted at "root".

    visit(obj, depth) is called once per node. Depth capped at max_depth — to prevent runaway walks over pathological
    trees."""

    def _walk(obj: Atspi.Accessible, depth: int) -> None:
        if obj is None or depth > max_depth:
            return
        visit(obj, depth)
        count = obj.get_child_count()
        for i in range(count):
            child = obj.get_child_at_index(i)
            if child is not None:
                _walk(child, depth + 1)

    _walk(root, 0)


def find_first_by_role(
    root: Atspi.Accessible,
    role_name: str,
    *,
    pred: Optional[Callable[[Atspi.Accessible], bool]] = None,
) -> Optional[Atspi.Accessible]:
    """Returns the first descendant (pre-order) whose role matches."""
    found: List[Atspi.Accessible] = []

    def _visit(obj: Atspi.Accessible, _depth: int) -> None:
        if found:
            return
        try:
            if obj.get_role_name() == role_name and (pred is None or pred(obj)):
                found.append(obj)
        except Exception:
            pass

    walk(root, _visit)
    return found[0] if found else None


def find_all_by_role(
    root: Atspi.Accessible,
    role_name: str,
    *,
    pred: Optional[Callable[[Atspi.Accessible], bool]] = None,
) -> List[Atspi.Accessible]:
    """Returns every descendant whose role matches, in pre-order."""
    result: List[Atspi.Accessible] = []

    def _visit(obj: Atspi.Accessible, _depth: int) -> None:
        try:
            if obj.get_role_name() == role_name and (pred is None or pred(obj)):
                result.append(obj)
        except Exception:
            pass

    walk(root, _visit)
    return result


def role_path(obj: Atspi.Accessible) -> str:
    """Return a slash-separated path of role names from app to obj, for diagnostics."""
    parts: List[str] = []
    cur = obj
    while cur is not None:
        try:
            parts.append(cur.get_role_name())
        except Exception:
            parts.append("?")
        try:
            parent = cur.get_parent()
        except Exception:
            parent = None
        if parent is None or parent == cur:
            break
        cur = parent
    return "/".join(reversed(parts))


def dump_subtree(root: Atspi.Accessible, *, max_depth: int = 10) -> str:
    """Return a human-readable indented dump of the subtree. Diagnostics only."""
    lines: List[str] = []

    def _visit(obj: Atspi.Accessible, depth: int) -> None:
        try:
            role = obj.get_role_name()
        except Exception:
            role = "?"
        try:
            name = obj.get_name()
        except Exception:
            name = "?"
        try:
            count = Atspi.Text.get_character_count(obj)
            text = Atspi.Text.get_text(obj, 0, count) if count else ""
        except Exception:
            text = ""
        indent = "  " * depth
        text_repr = repr(text[:60]) if text else ""
        lines.append(f"{indent}{role} name={name!r} text={text_repr}")

    walk(root, _visit, max_depth=max_depth)
    return "\n".join(lines)


def get_attributes_dict(obj: Atspi.Accessible) -> dict:
    """Return object attributes (e.g., "tag", "xml-role", "level") as a dict."""
    try:
        attrs = Atspi.Accessible.get_attributes(obj)
    except Exception:
        return {}
    if attrs is None:
        return {}
    # Atspi returns a GHashTable, which gi surfaces as a python dict.
    return dict(attrs)


def supports_interface(obj: Atspi.Accessible, iface_name: str) -> bool:
    """True if "obj" advertises the named AT-SPI2 interface.

    Example iface_name: "text", "action", "hypertext", "component", "value", "table", "tablecell"."""
    try:
        interfaces = Atspi.Accessible.get_interfaces(obj)
    except Exception:
        return False
    if interfaces is None:
        return False
    return iface_name.lower() in (s.lower() for s in interfaces)
