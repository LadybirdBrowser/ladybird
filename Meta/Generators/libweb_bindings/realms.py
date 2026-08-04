# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from typing import Union

from Utils.webidl_parser import Attribute
from Utils.webidl_parser import Interface
from Utils.webidl_parser import Operation
from Utils.webidl_parser import SpecialOperation


def member_realm_expr(
    member: Union[Attribute, Operation, SpecialOperation],
    *,
    interface: Interface,
    is_static: bool = False,
) -> str:
    """Return the C++ realm expression used for member work.

    See Meta/Generators/libweb_bindings/REALM_POLICY.md. Instance members
    default to the validated receiver realm. Statics, constructors, namespaces,
    and explicit caller-realm opt-outs use the caller realm.
    """
    if is_static or interface.is_namespace:
        return "realm"

    if "NeedsCallerRealm" in member.extended_attributes:
        return "realm"

    return "this_object_realm"


def member_passes_realm_to_implementation(member: Union[Attribute, Operation, SpecialOperation]) -> bool:
    return "NeedsCallerRealm" in member.extended_attributes
