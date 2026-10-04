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
from Generators.libweb_bindings.cpp_types import fully_qualified_name_for_interface
from Generators.libweb_bindings.cpp_types import idl_identifier_cpp_name
from Generators.libweb_bindings.global_mixins import global_mixin_header_is_provided_by_bindings
from Generators.libweb_bindings.global_mixins import write_global_mixin_declaration
from Generators.libweb_bindings.includes import GeneratedIncludes
from Generators.libweb_bindings.iterables import write_async_iterator_prototype_declaration
from Generators.libweb_bindings.iterables import write_iterator_prototype_declaration
from Generators.libweb_bindings.named_and_indexed_properties import interface_supports_named_properties
from Generators.libweb_bindings.named_and_indexed_properties import write_legacy_platform_object_function_declarations
from Generators.libweb_bindings.named_and_indexed_properties import write_named_properties_object_declaration
from Generators.libweb_bindings.namespaces import write_namespace_declaration
from Generators.libweb_bindings.overload_resolution import operation_callback_names
from Generators.libweb_bindings.wrappers import LOCATION_WRAPPER_HOOKS
from Generators.libweb_bindings.wrappers import interface_needs_wrapper
from Generators.libweb_bindings.wrappers import wrapper_base_class_name
from Generators.libweb_bindings.wrappers import wrapper_class_name
from Generators.libweb_bindings.wrappers import wrapper_host_class_name
from Utils.webidl_parser import Interface


def interface_is_location_object(interface: Interface) -> bool:
    return interface.name == "Location"


def interface_has_cross_origin_properties(interface: Interface) -> bool:
    return interface.name in ("Location", "Window")


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

    if interface_needs_wrapper(interface):
        includes.add("LibWeb/Bindings/PlatformObject.h")
        base_class = wrapper_base_class_name(context, interface)
        impl_type = fully_qualified_name_for_interface(interface)
        if interface.parent_name:
            parent_interface = context.interfaces.get(interface.parent_name)
            if parent_interface is not None:
                includes.add_binding(parent_interface.implemented_name)
        includes.add("LibJS/HostObjectABI.h")
        out.write(f"extern JSHostClass const {wrapper_host_class_name(interface)};\n")
        if interface_is_location_object(interface):
            out.write(f"extern JSHostObjectHooks const {LOCATION_WRAPPER_HOOKS};\n")
        out.write(
            f"""
class {wrapper_class_name(interface)} : public {base_class} {{
    WEB_PLATFORM_OBJECT({wrapper_class_name(interface)}, {base_class});
    GC_DECLARE_ALLOCATOR({wrapper_class_name(interface)});

public:
    {wrapper_class_name(interface)}(JS::Realm&, JSHostClass const&, GC::Ref<{impl_type}>);
    virtual ~{wrapper_class_name(interface)}() override;

    virtual void initialize(JS::Realm&) override;
"""
        )
        if interface_has_cross_origin_properties(interface):
            out.write(
                """
    static GC::Ref<JS::NativeFunction> create_cross_origin_method(JS::Realm&, Utf16FlyString const& property);
"""
            )
            if not interface_is_location_object(interface):
                out.write(
                    "    static GC::Ref<JS::NativeFunction> create_cross_origin_getter(JS::Realm&, Utf16FlyString const& property);\n"
                )
            out.write(
                "    static GC::Ref<JS::NativeFunction> create_cross_origin_setter(JS::Realm&, Utf16FlyString const& property);\n"
            )
        if interface_is_location_object(interface):
            out.write(
                """

    void initialize_location_object(JS::Realm&);
"""
            )
        if interface.name == "DOMException":
            out.write(
                """    virtual JS::ErrorData* error_data() override;
    virtual JS::ErrorData const* error_data() const override;
"""
            )
        out.write("\nprotected:\n")
        out.write(f"    {impl_type}& impl();\n")
        out.write(f"    {impl_type} const& impl() const;\n")
        out.write("};\n\n")

        write_legacy_platform_object_function_declarations(out, interface)

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
    out.write(
        f"""struct {interface.prototype_class} {{
public:
    static void initialize(JS::Realm&, JS::Object&);
"""
    )
    # NB: The unforgeable attributes of a [Global] interface live on the global object, which its global mixin defines.
    if "Global" not in interface.extended_attributes:
        out.write("    static void define_unforgeable_attributes(JS::Realm&, JS::Object&);\n")
    out.write(
        """
private:
"""
    )
    if interface_has_cross_origin_properties(interface):
        out.write(f"    friend class {wrapper_class_name(interface)};\n\n")
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
    if "Global" in interface.extended_attributes and global_mixin_header_is_provided_by_bindings(interface):
        includes.add_binding(f"{interface.name}GlobalMixin")
    elif "Global" in interface.extended_attributes:
        write_global_mixin_declaration(out, context, interface)
    if interface_supports_named_properties(interface):
        write_named_properties_object_declaration(out, includes, interface)
    write_iterator_prototype_declaration(out, interface)
    write_async_iterator_prototype_declaration(out, interface)
