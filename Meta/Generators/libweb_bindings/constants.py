# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause


from typing import TextIO

from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.includes import GeneratedIncludes
from Generators.libweb_bindings.to_js_value import to_javascript_value
from Utils.webidl_parser import Interface


def define_the_constants(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
) -> None:
    if not interface.constants:
        return
    includes.add("LibJS/Runtime/PropertyDescriptor.h")
    out.write("\n")

    # 1. For each constant const that is a member of definition:
    for constant in interface.constants:
        out.write(
            f"""    // 1. FIXME: If const is not exposed in realm, then continue.
    // 2. Let value be the result of converting const’s IDL value to a JavaScript value.
    // 3. Let desc be the PropertyDescriptor{{[[Writable]]: false, [[Enumerable]]: true, [[Configurable]]: false, [[Value]]: value}}.
    // 4. Let id be const’s identifier.
    // 5. Perform ! DefinePropertyOrThrow(target, id, desc).
    object.define_direct_property("{constant.name}"_utf16_fly_string, {to_javascript_value(constant.type, constant.value, includes, context)}, JS::Attribute::Enumerable);
"""
        )
