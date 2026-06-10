# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from Generators.libweb_bindings.context import GenerationContext
from Utils.webidl_parser import Interface


def interface_can_have_instances(interface: Interface) -> bool:
    return (
        bool(interface.parent_name)
        or bool(interface.constructors)
        or "LegacyNoInterfaceObject" in interface.extended_attributes
        or bool(interface.regular_attributes)
        or bool(interface.static_attributes)
        or bool(interface.regular_operations)
        or bool(interface.static_operations)
        or interface.stringifier is not None
        or interface.iterable is not None
        or interface.async_iterable is not None
        or interface.maplike is not None
        or interface.setlike is not None
        or interface.indexed_property_getter is not None
        or interface.named_property_getter is not None
        or interface.named_property_setter is not None
        or interface.named_property_deleter is not None
        or interface.indexed_property_setter is not None
        or not bool(interface.constants)
    )


def interface_needs_wrapper(interface: Interface) -> bool:
    return (
        not interface.is_namespace and not interface.is_callback_interface and interface_can_have_instances(interface)
    )


def wrapper_needs_wrappable_impl(context: GenerationContext, interface: Interface) -> bool:
    if not interface_needs_wrapper(interface):
        return False

    if not interface.parent_name:
        return True

    parent_interface = context.interfaces.get(interface.parent_name)
    if parent_interface is None:
        raise RuntimeError(f"Interface '{interface.name}' inherits from unknown interface '{interface.parent_name}'")

    return not interface_needs_wrapper(parent_interface)


def wrapper_class_name(interface: Interface) -> str:
    return f"{interface.implemented_name}Wrapper"


def wrapper_base_class_name(context: GenerationContext, interface: Interface) -> str:
    if not interface.parent_name:
        return "PlatformObject"

    parent_interface = context.interfaces.get(interface.parent_name)
    if parent_interface is None:
        raise RuntimeError(f"Interface '{interface.name}' inherits from unknown interface '{interface.parent_name}'")

    return wrapper_class_name(parent_interface)


def has_legacy_override_built_ins_interface_extended_attribute(interface: Interface) -> bool:
    return (
        "LegacyOverrideBuiltIns" in interface.extended_attributes
        or "LegacyOverrideBuiltins" in interface.extended_attributes
    )


def needs_legacy_platform_object_flags_initialization(interface: Interface) -> bool:
    return (
        interface.indexed_property_getter is not None
        or interface.named_property_getter is not None
        or interface.indexed_property_setter is not None
        or interface.named_property_setter is not None
        or interface.named_property_deleter is not None
        or "LegacyUnenumerableNamedProperties" in interface.extended_attributes
        or has_legacy_override_built_ins_interface_extended_attribute(interface)
        or "Global" in interface.extended_attributes
    )
