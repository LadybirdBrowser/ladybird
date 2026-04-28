"""NSAccessibility tree walking and query helpers."""

from __future__ import annotations

import time

from typing import Callable
from typing import List
from typing import Optional

from ApplicationServices import AXUIElementCopyAttributeNames
from ApplicationServices import AXUIElementCopyAttributeValue
from ApplicationServices import AXUIElementCopyParameterizedAttributeNames
from ApplicationServices import AXUIElementCopyParameterizedAttributeValue
from ApplicationServices import kAXChildrenAttribute
from ApplicationServices import kAXRoleAttribute
from ApplicationServices import kAXSubroleAttribute
from ApplicationServices import kAXTitleAttribute
from ApplicationServices import kAXValueAttribute


def _ax_attr(elem, key):
    """Read one AX attribute. Returns None on any error."""
    if elem is None:
        return None
    try:
        err, value = AXUIElementCopyAttributeValue(elem, key, None)
    except Exception:
        return None
    return None if err != 0 else value


def walk(
    root,
    visit: Callable[[object, int], None],
    *,
    max_depth: int = 30,
) -> None:
    """Pre-order DFS of the AX subtree rooted at "root".

    visit(obj, depth) is called once per node. Depth capped at max_depth — to prevent runaway walks over pathological
    trees."""

    def _walk(obj, depth: int) -> None:
        if obj is None or depth > max_depth:
            return
        visit(obj, depth)
        for child in _ax_attr(obj, kAXChildrenAttribute) or []:
            _walk(child, depth + 1)

    _walk(root, 0)


def find_first_by_role(
    root,
    role_name: str,
    *,
    pred: Optional[Callable[[object], bool]] = None,
):
    """Returns the first descendant (pre-order) whose AXRole matches.

    role_name examples: "AXWebArea", "AXHeading", "AXLink", "AXStaticText", "AXGroup", "AXButton"."""
    found: List[object] = []

    def _visit(obj, _depth: int) -> None:
        if found:
            return
        try:
            if _ax_attr(obj, kAXRoleAttribute) == role_name and (pred is None or pred(obj)):
                found.append(obj)
        except Exception:
            pass

    walk(root, _visit)
    return found[0] if found else None


def find_all_by_role(
    root,
    role_name: str,
    *,
    pred: Optional[Callable[[object], bool]] = None,
) -> List[object]:
    """Returns every descendant whose AXRole matches, in pre-order."""
    result: List[object] = []

    def _visit(obj, _depth: int) -> None:
        try:
            if _ax_attr(obj, kAXRoleAttribute) == role_name and (pred is None or pred(obj)):
                result.append(obj)
        except Exception:
            pass

    walk(root, _visit)
    return result


def wait_for_descendant_by_role(
    root,
    role_name: str,
    *,
    title: Optional[str] = None,
    value: Optional[str] = None,
    pred: Optional[Callable[[object], bool]] = None,
    timeout: float = 5.0,
    interval: float = 0.1,
):
    """Polling variant of find_first_by_role — handles trees that are still populating.

    Right after Ladybird launches, the AXWebArea can be visible before all of its descendants are exposed. A plain
    find_first_by_role call walks the current state once and misses nodes that arrive a moment later — which manifests
    as flaky "expected foo in the tree" failures when test cases run back-to-back. This helper polls find_first_by_role
    until either it returns a match, or else the timeout expires.

    "title", "value", and "pred" compose with the role match: if multiple are given, the node must match all of them."""

    def combined(obj) -> bool:
        if title is not None:
            try:
                if _ax_attr(obj, kAXTitleAttribute) != title:
                    return False
            except Exception:
                return False
        if value is not None:
            try:
                if _ax_attr(obj, kAXValueAttribute) != value:
                    return False
            except Exception:
                return False
        return pred is None or pred(obj)

    deadline = time.monotonic() + timeout
    while True:
        hit = find_first_by_role(root, role_name, pred=combined)
        if hit is not None:
            return hit
        if time.monotonic() >= deadline:
            return None
        time.sleep(interval)


def role_path(obj) -> str:
    """Return a slash-separated path of role names from app to obj, for diagnostics."""
    parts: List[str] = []
    cur = obj
    while cur is not None:
        try:
            parts.append(_ax_attr(cur, kAXRoleAttribute) or "?")
        except Exception:
            parts.append("?")
        try:
            from ApplicationServices import kAXParentAttribute

            parent = _ax_attr(cur, kAXParentAttribute)
        except Exception:
            parent = None
        if parent is None or parent == cur:
            break
        cur = parent
    return "/".join(reversed(parts))


def dump_subtree(root, *, max_depth: int = 10) -> str:
    """Return a human-readable indented dump of the subtree. Diagnostics only."""
    lines: List[str] = []

    def _visit(obj, depth: int) -> None:
        role = _ax_attr(obj, kAXRoleAttribute) or "?"
        sub = _ax_attr(obj, kAXSubroleAttribute)
        title = _ax_attr(obj, kAXTitleAttribute)
        value = _ax_attr(obj, kAXValueAttribute)
        indent = "  " * depth
        bits = [role]
        if sub:
            bits.append(f"sub={sub}")
        if title:
            bits.append(f"title={title!r}")
        if isinstance(value, str) and value:
            bits.append(f"value={value!r}")
        lines.append(indent + " ".join(bits))

    walk(root, _visit, max_depth=max_depth)
    return "\n".join(lines)


def get_attribute_names(obj) -> List[str]:
    """Return the list of attribute names this element advertises."""
    if obj is None:
        return []
    try:
        err, names = AXUIElementCopyAttributeNames(obj, None)
    except Exception:
        return []
    return list(names) if err == 0 and names else []


def get_parameterized_attribute_names(obj) -> List[str]:
    """Return the list of parameterized attribute names this element advertises."""
    if obj is None:
        return []
    try:
        err, names = AXUIElementCopyParameterizedAttributeNames(obj, None)
    except Exception:
        return []
    return list(names) if err == 0 and names else []


def supports_attribute(obj, attribute_name: str) -> bool:
    """True if "obj" advertises the named NSAccessibility attribute."""
    return attribute_name in get_attribute_names(obj)


def get_parameterized_attribute_value(obj, attribute_name: str, parameter):
    """Read a parameterized attribute. Returns None on any error."""
    if obj is None:
        return None
    try:
        err, value = AXUIElementCopyParameterizedAttributeValue(obj, attribute_name, parameter, None)
    except Exception:
        return None
    return None if err != 0 else value
