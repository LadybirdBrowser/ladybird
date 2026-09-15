# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from typing import Literal

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
