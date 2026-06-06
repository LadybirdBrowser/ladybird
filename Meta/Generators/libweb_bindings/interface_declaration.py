# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause


from typing import TextIO

from Generators.libweb_bindings import overload_resolution
from Generators.libweb_bindings.attributes import attribute_getter_callback_name
from Generators.libweb_bindings.attributes import attribute_has_setter
from Generators.libweb_bindings.attributes import attribute_setter_callback_name
from Generators.libweb_bindings.callback_interfaces import write_callback_interface_declaration
from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.cpp_types import idl_identifier_cpp_name
from Generators.libweb_bindings.global_mixins import write_global_mixin_declaration
from Generators.libweb_bindings.includes import GeneratedIncludes
from Generators.libweb_bindings.iterables import write_async_iterator_prototype_declaration
from Generators.libweb_bindings.iterables import write_iterator_prototype_declaration
from Generators.libweb_bindings.named_and_indexed_properties import interface_supports_named_properties
from Generators.libweb_bindings.named_and_indexed_properties import write_named_properties_object_declaration
from Generators.libweb_bindings.namespaces import write_namespace_declaration
from Generators.libweb_bindings.overload_resolution import operation_callback_names
from Utils.webidl_parser import Interface


def interface_requires_custom_prototype(interface: Interface) -> bool:
    return (
        "Global" in interface.extended_attributes
        or interface.indexed_property_getter is not None
        or interface.named_property_getter is not None
        or interface.named_property_setter is not None
        or interface.named_property_deleter is not None
        or interface.indexed_property_setter is not None
        or interface.maplike is not None
        or interface.setlike is not None
        or (interface.iterable is not None and interface.iterable.key_type is not None)
        or interface.async_iterable is not None
    )


def write_declaration(
    out: TextIO, includes: GeneratedIncludes, context: GenerationContext, interface: Interface
) -> None:
    if interface.is_callback_interface:
        write_callback_interface_declaration(out, includes, context, interface)
        return

    if interface.is_namespace:
        write_namespace_declaration(out, includes, context, interface)
        return

    includes.add("LibJS/Runtime/NativeFunction.h")
    includes.add("LibJS/Runtime/Object.h")
    includes.add("LibWeb/Bindings/InterfaceObject.h")
    operation_callbacks = operation_callback_names(interface)

    out.write(
        f"""struct {interface.constructor_class} {{
public:
    static void initialize(JS::Realm&, JS::NativeFunction&);
    static JS::ThrowCompletionOr<GC::Ref<JS::Object>> construct(InterfaceConstructor&, JS::FunctionObject&);

private:
"""
    )
    if len(interface.constructors) > 1:
        for overload_index, _ in enumerate(interface.constructors):
            out.write(
                f"    static JS::ThrowCompletionOr<GC::Ref<JS::Object>> construct{overload_index}(InterfaceConstructor&, JS::FunctionObject&);\n"
            )
    for operations in overload_resolution.operation_overload_sets(interface, static=True).values():
        operation = operations[0]
        out.write(f"    JS_DECLARE_NATIVE_FUNCTION({idl_identifier_cpp_name(operation)});\n")
        if len(operations) > 1:
            for overload_index, overloaded_operation in enumerate(operations):
                out.write(
                    f"    JS_DECLARE_NATIVE_FUNCTION({idl_identifier_cpp_name(overloaded_operation, suffix=overload_index)});\n"
                )
    for attribute in interface.static_attributes:
        if "FIXME" in attribute.extended_attributes:
            continue
        out.write(f"    JS_DECLARE_NATIVE_FUNCTION({attribute_getter_callback_name(attribute)});\n")
    out.write(
        """\
};

"""
    )
    if interface_requires_custom_prototype(interface):
        out.write(
            f"""class {interface.prototype_class} : public JS::Object {{
    JS_OBJECT({interface.prototype_class}, JS::Object);
    GC_DECLARE_ALLOCATOR({interface.prototype_class});

public:
    static void define_unforgeable_attributes(JS::Realm&, JS::Object&);

    explicit {interface.prototype_class}(JS::Realm&);
    virtual void initialize(JS::Realm&) override;
    virtual ~{interface.prototype_class}() override;
"""
        )
        if "Global" in interface.extended_attributes:
            out.write("    virtual JS::ThrowCompletionOr<bool> internal_set_prototype_of(JS::Object*) override;\n")
        out.write(
            """
private:
"""
        )
    else:
        out.write(
            f"""struct {interface.prototype_class} {{
public:
    static void initialize(JS::Realm&, JS::Object&);
    static void define_unforgeable_attributes(JS::Realm&, JS::Object&);

private:
"""
        )
    for attribute in interface.regular_attributes:
        if "FIXME" in attribute.extended_attributes:
            continue
        out.write(f"    JS_DECLARE_NATIVE_FUNCTION({attribute_getter_callback_name(attribute)});\n")
        if attribute_has_setter(attribute):
            out.write(f"    JS_DECLARE_NATIVE_FUNCTION({attribute_setter_callback_name(attribute)});\n")
    for operations in overload_resolution.operation_overload_sets(interface).values():
        operation = operations[0]
        out.write(f"    JS_DECLARE_NATIVE_FUNCTION({idl_identifier_cpp_name(operation)});\n")
        if len(operations) > 1:
            for overload_index, overloaded_operation in enumerate(operations):
                out.write(
                    f"    JS_DECLARE_NATIVE_FUNCTION({idl_identifier_cpp_name(overloaded_operation, suffix=overload_index)});\n"
                )
    if interface.stringifier is not None:
        out.write("    JS_DECLARE_NATIVE_FUNCTION(to_string);\n")
    if interface.indexed_property_getter is not None and interface.indexed_property_getter.name:
        out.write(f"    JS_DECLARE_NATIVE_FUNCTION({idl_identifier_cpp_name(interface.indexed_property_getter)});\n")
    if interface.named_property_getter is not None and interface.named_property_getter.name:
        out.write(f"    JS_DECLARE_NATIVE_FUNCTION({idl_identifier_cpp_name(interface.named_property_getter)});\n")
    if interface.named_property_setter is not None and interface.named_property_setter.name:
        out.write(f"    JS_DECLARE_NATIVE_FUNCTION({idl_identifier_cpp_name(interface.named_property_setter)});\n")
    if interface.named_property_deleter is not None and interface.named_property_deleter.name:
        out.write(f"    JS_DECLARE_NATIVE_FUNCTION({idl_identifier_cpp_name(interface.named_property_deleter)});\n")
    if interface.maplike is not None:
        out.write("    JS_DECLARE_NATIVE_FUNCTION(get_size);\n")
        out.write("    JS_DECLARE_NATIVE_FUNCTION(entries);\n")
        out.write("    JS_DECLARE_NATIVE_FUNCTION(keys);\n")
        out.write("    JS_DECLARE_NATIVE_FUNCTION(values);\n")
        out.write("    JS_DECLARE_NATIVE_FUNCTION(for_each);\n")
        out.write("    JS_DECLARE_NATIVE_FUNCTION(get);\n")
        out.write("    JS_DECLARE_NATIVE_FUNCTION(has);\n")
        if not interface.maplike.readonly:
            out.write("    JS_DECLARE_NATIVE_FUNCTION(delete_);\n")
            out.write("    JS_DECLARE_NATIVE_FUNCTION(clear);\n")
    if interface.setlike is not None:
        out.write("    JS_DECLARE_NATIVE_FUNCTION(get_size);\n")
        out.write("    JS_DECLARE_NATIVE_FUNCTION(entries);\n")
        out.write("    JS_DECLARE_NATIVE_FUNCTION(values);\n")
        out.write("    JS_DECLARE_NATIVE_FUNCTION(for_each);\n")
        out.write("    JS_DECLARE_NATIVE_FUNCTION(has);\n")
        if not interface.setlike.readonly:
            if "add" not in operation_callbacks:
                out.write("    JS_DECLARE_NATIVE_FUNCTION(add);\n")
            if "delete_" not in operation_callbacks:
                out.write("    JS_DECLARE_NATIVE_FUNCTION(delete_);\n")
            if "clear" not in operation_callbacks:
                out.write("    JS_DECLARE_NATIVE_FUNCTION(clear);\n")
    if interface.iterable is not None and interface.iterable.key_type is not None:
        out.write("    JS_DECLARE_NATIVE_FUNCTION(entries);\n")
        out.write("    JS_DECLARE_NATIVE_FUNCTION(for_each);\n")
        out.write("    JS_DECLARE_NATIVE_FUNCTION(keys);\n")
        out.write("    JS_DECLARE_NATIVE_FUNCTION(values);\n")
    if interface.async_iterable is not None:
        out.write("    JS_DECLARE_NATIVE_FUNCTION(values);\n")
    out.write(
        """};

"""
    )
    if "Global" in interface.extended_attributes:
        write_global_mixin_declaration(out, context, interface)
    if interface_supports_named_properties(interface):
        write_named_properties_object_declaration(out, includes, interface)
    write_iterator_prototype_declaration(out, interface)
    write_async_iterator_prototype_declaration(out, interface)
