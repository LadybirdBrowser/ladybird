# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause


from typing import TextIO

from Generators.libweb_bindings import overload_resolution
from Generators.libweb_bindings.attributes import attribute_getter_callback_name
from Generators.libweb_bindings.attributes import attribute_has_setter
from Generators.libweb_bindings.attributes import attribute_setter_callback_name
from Generators.libweb_bindings.attributes import define_the_regular_attributes
from Generators.libweb_bindings.attributes import define_the_unforgeable_attributes
from Generators.libweb_bindings.attributes import write_attribute_getter
from Generators.libweb_bindings.attributes import write_attribute_setter
from Generators.libweb_bindings.constants import define_the_constants
from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.cpp_types import idl_identifier_cpp_name
from Generators.libweb_bindings.includes import GeneratedIncludes
from Generators.libweb_bindings.operations import define_the_regular_operations
from Generators.libweb_bindings.operations import define_the_stringifier
from Generators.libweb_bindings.operations import write_regular_operations_for_receiver
from Generators.libweb_bindings.operations import write_stringifier
from Utils.webidl_parser import Interface


def global_mixin_member_interfaces(interface: Interface, context: GenerationContext) -> list[Interface]:
    return [
        interface_in_chain
        for interface_in_chain in reversed(context.inheritance_stack(interface))
        if interface_in_chain.name != "EventTarget"
    ]


def write_global_mixin_declaration(out: TextIO, context: GenerationContext, interface: Interface) -> None:
    out.write(
        f"""class {interface.name}GlobalMixin {{
public:
    void initialize(JS::Realm&, JS::Object&);
    void define_unforgeable_attributes(JS::Realm&, JS::Object&);
    {interface.name}GlobalMixin();
    virtual ~{interface.name}GlobalMixin();

private:
"""
    )
    declared_callbacks: set[str] = set()
    for member_interface in global_mixin_member_interfaces(interface, context):
        for attribute in member_interface.regular_attributes:
            if "FIXME" in attribute.extended_attributes:
                continue
            getter_callback = attribute_getter_callback_name(attribute)
            if getter_callback not in declared_callbacks:
                declared_callbacks.add(getter_callback)
                out.write(f"    JS_DECLARE_NATIVE_FUNCTION({getter_callback});\n")
            if attribute_has_setter(attribute, include_replaceable=True):
                setter_callback = attribute_setter_callback_name(attribute)
                if setter_callback not in declared_callbacks:
                    declared_callbacks.add(setter_callback)
                    out.write(f"    JS_DECLARE_NATIVE_FUNCTION({setter_callback});\n")
        for operations in overload_resolution.operation_overload_sets(member_interface).values():
            operation = operations[0]
            callback = idl_identifier_cpp_name(operation)
            if callback not in declared_callbacks:
                declared_callbacks.add(callback)
                out.write(f"    JS_DECLARE_NATIVE_FUNCTION({callback});\n")
            if len(operations) > 1:
                for overload_index, overloaded_operation in enumerate(operations):
                    overloaded_callback = idl_identifier_cpp_name(overloaded_operation, suffix=overload_index)
                    if overloaded_callback not in declared_callbacks:
                        declared_callbacks.add(overloaded_callback)
                        out.write(f"    JS_DECLARE_NATIVE_FUNCTION({overloaded_callback});\n")
        if member_interface.stringifier is not None and "to_string" not in declared_callbacks:
            declared_callbacks.add("to_string")
            out.write("    JS_DECLARE_NATIVE_FUNCTION(to_string);\n")
    out.write(
        """};

"""
    )


def write_global_mixin_implementation(
    out: TextIO, context: GenerationContext, includes: GeneratedIncludes, interface: Interface
) -> None:
    if "Global" not in interface.extended_attributes:
        return

    includes.add("LibWeb/Bindings/Intrinsics.h")

    member_interfaces = global_mixin_member_interfaces(interface, context)
    out.write(
        f"""{interface.name}GlobalMixin::{interface.name}GlobalMixin() = default;
{interface.name}GlobalMixin::~{interface.name}GlobalMixin() = default;

void {interface.name}GlobalMixin::initialize(JS::Realm& realm, [[maybe_unused]] JS::Object& object)
{{
    [[maybe_unused]] auto& vm = realm.vm();
    [[maybe_unused]] u8 default_attributes = JS::Attribute::Enumerable | JS::Attribute::Configurable | JS::Attribute::Writable;

    object.set_prototype(&ensure_web_prototype<{interface.prototype_class}>(realm, "{interface.namespaced_name}"_fly_string));
"""
    )
    for member_interface in member_interfaces:
        define_the_regular_attributes(out, includes, member_interface, include_replaceable_setters=True)
        define_the_regular_operations(out, includes, member_interface)
        define_the_stringifier(out, includes, member_interface)
        define_the_constants(out, context, includes, member_interface)
    out.write(
        f"""}}

void {interface.name}GlobalMixin::define_unforgeable_attributes(JS::Realm& realm, [[maybe_unused]] JS::Object& object)
{{
    [[maybe_unused]] auto& vm = realm.vm();
    [[maybe_unused]] u8 default_attributes = JS::Attribute::Enumerable;
"""
    )
    for member_interface in member_interfaces:
        define_the_unforgeable_attributes(out, includes, member_interface, include_replaceable_setters=True)
        define_the_regular_operations(out, includes, member_interface, unforgeable=True)
        define_the_stringifier(out, includes, member_interface, unforgeable=True)
    out.write(
        """}

"""
    )

    defined_callbacks: set[str] = set()
    for member_interface in member_interfaces:
        for attribute in member_interface.regular_attributes:
            if "FIXME" in attribute.extended_attributes:
                continue
            getter_callback = attribute_getter_callback_name(attribute)
            if getter_callback not in defined_callbacks:
                defined_callbacks.add(getter_callback)
                write_attribute_getter(
                    out, context, includes, member_interface, attribute, f"{interface.name}GlobalMixin"
                )
            if attribute_has_setter(attribute, include_replaceable=True):
                setter_callback = attribute_setter_callback_name(attribute)
                if setter_callback not in defined_callbacks:
                    defined_callbacks.add(setter_callback)
                    write_attribute_setter(
                        out, context, includes, member_interface, attribute, f"{interface.name}GlobalMixin"
                    )
        write_regular_operations_for_receiver(
            out, context, includes, member_interface, f"{interface.name}GlobalMixin", defined_callbacks
        )
        if member_interface.stringifier is not None and "to_string" not in defined_callbacks:
            defined_callbacks.add("to_string")
            write_stringifier(out, context, includes, member_interface, f"{interface.name}GlobalMixin")
