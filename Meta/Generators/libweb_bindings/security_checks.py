# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from typing import Literal
from typing import Optional

from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.includes import GeneratedIncludes
from Utils.webidl_parser import Interface

SecurityCheckType = Literal["method", "getter", "setter"]


def interface_needs_security_check(context: GenerationContext, interface: Interface) -> bool:
    # NOTE: The HTML Standard's security check returns early for any platform object that is not a Window or Location
    #       object, so it only needs to be performed for members of interfaces that those objects implement.
    for platform_interface_name in ("Window", "Location"):
        platform_interface = context.interfaces.get(platform_interface_name)
        if platform_interface is None:
            continue
        if any(
            interface_in_chain.name == interface.name
            for interface_in_chain in context.inheritance_stack(platform_interface)
        ):
            return True
    return False


# https://webidl.spec.whatwg.org/#dfn-perform-a-security-check
def perform_a_security_check(
    includes: GeneratedIncludes,
    js_value: str,
    identifier: str,
    type_: SecurityCheckType,
) -> str:
    includes.add("LibWeb/HTML/CrossOrigin/AbstractOperations.h")
    return f'TRY(HTML::perform_a_security_check(vm, {js_value}, u"{identifier}"sv, HTML::SecurityCheckType::{type_.capitalize()}));'


# https://html.spec.whatwg.org/multipage/nav-history-apis.html#crossoriginproperties-(-o-)
# CrossOriginProperties(O) for a Window object O, as [[NeedsGet]] and [[NeedsSet]], or None for an operation.
CROSS_ORIGIN_WINDOW_PROPERTIES: dict[str, Optional[tuple[bool, bool]]] = {
    "window": (True, False),
    "self": (True, False),
    "location": (True, True),
    "close": None,
    "closed": (True, False),
    "focus": None,
    "blur": None,
    "frames": (True, False),
    "length": (True, False),
    "top": (True, False),
    "opener": (True, False),
    "parent": (True, False),
    "postMessage": None,
}


def window_member_is_cross_origin_accessible(interface: Interface, identifier: str, type_: SecurityCheckType) -> bool:
    if interface.name != "Window" or identifier not in CROSS_ORIGIN_WINDOW_PROPERTIES:
        return False
    entry = CROSS_ORIGIN_WINDOW_PROPERTIES[identifier]
    if type_ == "method":
        return entry is None
    if type_ == "getter":
        return entry is not None and entry[0]
    return entry is not None and entry[1]


def the_remote_window(includes: GeneratedIncludes, js_value: str) -> str:
    # NOTE: A WindowProxy's [[Window]] may be a RemoteWindow standing for a Window hosted by another process. The
    #       security check only lets a cross-origin accessible member through for it, and RemoteWindow declares that
    #       member as Window does, so the steps below run on whichever of the two jsValue's [[Window]] is.
    includes.add("LibWeb/HTML/CrossOrigin/AbstractOperations.h")
    includes.add("LibWeb/HTML/RemoteWindow.h")
    return f"auto remote_window = HTML::remote_window_from({js_value});"


def on_the_window(call: str) -> str:
    return f"HTML::invoke_on_window(idl_object, remote_window.ptr(), [&](auto& window) {{ return {call}; }})"
