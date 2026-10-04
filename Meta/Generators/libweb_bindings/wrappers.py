# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from typing import Optional

from Generators.libweb_bindings.context import GenerationContext
from Utils.utils import title_case_to_snake_case
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


def wrapper_host_class_name(interface: Interface) -> str:
    return f"{title_case_to_snake_case(wrapper_class_name(interface))}_host_class"


def wrapper_legacy_platform_object_info_name(interface: Interface) -> str:
    return f"{title_case_to_snake_case(wrapper_class_name(interface))}_legacy_platform_object_info"


def parent_interface(context: GenerationContext, interface: Interface) -> Optional[Interface]:
    if not interface.parent_name:
        return None

    parent = context.interfaces.get(interface.parent_name)
    if parent is None:
        raise RuntimeError(f"Interface '{interface.name}' inherits from unknown interface '{interface.parent_name}'")
    return parent


def interface_and_inherited_interfaces(context: GenerationContext, interface: Interface) -> list[Interface]:
    interfaces = []
    current: Optional[Interface] = interface
    while current is not None:
        interfaces.append(current)
        current = parent_interface(context, current)
    return interfaces


def has_legacy_override_built_ins_interface_extended_attribute(interface: Interface) -> bool:
    return "LegacyOverrideBuiltIns" in interface.extended_attributes


# The fields of LegacyPlatformObjectInfo (LibWeb/Bindings/PlatformObject.h), in declaration order.
LEGACY_PLATFORM_OBJECT_INFO_FIELDS = (
    "supports_indexed_properties",
    "supports_named_properties",
    "has_indexed_property_setter",
    "has_named_property_setter",
    "has_named_property_deleter",
    "has_legacy_unenumerable_named_properties_interface_extended_attribute",
    "has_legacy_override_built_ins_interface_extended_attribute",
    "has_global_interface_extended_attribute",
    "indexed_property_setter_has_identifier",
    "named_property_setter_has_identifier",
    "named_property_deleter_has_identifier",
)


def legacy_platform_object_info_fields_declared_by(interface: Interface) -> set[str]:
    fields = set()
    if interface.indexed_property_getter is not None:
        fields.add("supports_indexed_properties")
    if interface.named_property_getter is not None:
        fields.add("supports_named_properties")
    if interface.indexed_property_setter is not None:
        fields.add("has_indexed_property_setter")
        if interface.indexed_property_setter.name:
            fields.add("indexed_property_setter_has_identifier")
    if interface.named_property_setter is not None:
        fields.add("has_named_property_setter")
        if interface.named_property_setter.name:
            fields.add("named_property_setter_has_identifier")
    if interface.named_property_deleter is not None:
        fields.add("has_named_property_deleter")
        if interface.named_property_deleter.name:
            fields.add("named_property_deleter_has_identifier")
    if "LegacyUnenumerableNamedProperties" in interface.extended_attributes:
        fields.add("has_legacy_unenumerable_named_properties_interface_extended_attribute")
    if has_legacy_override_built_ins_interface_extended_attribute(interface):
        fields.add("has_legacy_override_built_ins_interface_extended_attribute")
    if "Global" in interface.extended_attributes:
        fields.add("has_global_interface_extended_attribute")
    return fields


# The LegacyPlatformObjectInfo fields that are true for a wrapper, or None if the wrapper is not a legacy platform
# object. A wrapper is one as soon as its interface or an inherited interface declares any of the fields.
def legacy_platform_object_info_fields(context: GenerationContext, interface: Interface) -> Optional[list[str]]:
    fields: Optional[set[str]] = None
    for interface_in_chain in interface_and_inherited_interfaces(context, interface):
        declared_fields = legacy_platform_object_info_fields_declared_by(interface_in_chain)
        if declared_fields:
            fields = (fields or set()) | declared_fields
    if fields is None:
        return None
    return [field for field in LEGACY_PLATFORM_OBJECT_INFO_FIELDS if field in fields]


def wrapper_host_class_flags(context: GenerationContext, interface: Interface) -> list[str]:
    interfaces = interface_and_inherited_interfaces(context, interface)
    is_location = any(interface_in_chain.name == "Location" for interface_in_chain in interfaces)
    is_global = any("Global" in interface_in_chain.extended_attributes for interface_in_chain in interfaces)
    is_legacy_platform_object = legacy_platform_object_info_fields(context, interface) is not None

    flags = ["platform_object_host_class_flags"]
    if is_location:
        flags.append("JS_HOST_CLASS_MAY_INTERFERE_WITH_INDEXED_PROPERTY_ACCESS")
    if any(interface_in_chain.name == "HTMLAllCollection" for interface_in_chain in interfaces):
        flags.append("JS_HOST_CLASS_IS_HTMLDDA")
    if is_global:
        flags.append("JS_HOST_CLASS_IS_GLOBAL_OBJECT")
    if is_global or is_location:
        flags.append("JS_HOST_CLASS_IMMUTABLE_PROTOTYPE")
    if is_legacy_platform_object or is_location:
        flags.append("JS_HOST_CLASS_NOT_CACHEABLE_FOR_PROPERTY_ABSENCE")
    return flags


# Location's internal methods are overrides of LocationWrapper in LibWeb/HTML/Location.cpp, which also defines its hooks.
LOCATION_WRAPPER_HOOKS = "location_wrapper_hooks"


def wrapper_host_class_hooks(context: GenerationContext, interface: Interface) -> str:
    if interface.name == "Location":
        return LOCATION_WRAPPER_HOOKS
    legacy_platform_object_info_fields_of_wrapper = legacy_platform_object_info_fields(context, interface)
    if legacy_platform_object_info_fields_of_wrapper is None:
        return "platform_object_hooks"
    if "has_global_interface_extended_attribute" in legacy_platform_object_info_fields_of_wrapper:
        return "global_platform_object_hooks"
    return "legacy_platform_object_hooks"
