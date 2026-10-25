# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause


from typing import TextIO

from Generators.libweb_bindings import overload_resolution
from Generators.libweb_bindings.attributes import attribute_getter_callback_name
from Generators.libweb_bindings.attributes import define_the_regular_attributes
from Generators.libweb_bindings.constants import define_the_constants
from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.cpp_types import fully_qualified_name_for_interface
from Generators.libweb_bindings.cpp_types import idl_identifier_cpp_name
from Generators.libweb_bindings.cpp_types import idl_implementation_cpp_name
from Generators.libweb_bindings.cpp_types import implementation_header_for_interface
from Generators.libweb_bindings.includes import GeneratedIncludes
from Generators.libweb_bindings.operations import define_the_regular_operations
from Generators.libweb_bindings.operations import write_regular_operations
from Generators.libweb_bindings.to_js_value import to_javascript_value
from Utils.webidl_parser import Interface

UNSUPPORTED_NAMESPACE_EXTENDED_ATTRIBUTES = ("WithGCVisitor", "WithFinalizer")


def write_namespace_declaration(
    out: TextIO, includes: GeneratedIncludes, context: GenerationContext, interface: Interface
) -> None:
    for extended_attribute in UNSUPPORTED_NAMESPACE_EXTENDED_ATTRIBUTES:
        if extended_attribute in interface.extended_attributes:
            raise RuntimeError(
                f"Namespace {interface.name} has [{extended_attribute}], but namespace objects are ordinary objects"
                " that cannot carry native state. Keep that state in a GC cell owned by the realm instead."
            )

    includes.add("LibJS/Runtime/NativeFunction.h")
    includes.add("LibJS/Runtime/Object.h")

    out.write(
        f"""struct {interface.namespace_class} {{
    static void initialize(JS::Realm&, JS::Object&);

private:
"""
    )
    for attribute in namespace_attributes(interface):
        out.write(f"    JS_DECLARE_NATIVE_FUNCTION({attribute_getter_callback_name(attribute)});\n")
    for operations in overload_resolution.operation_overload_sets(interface).values():
        operation = operations[0]
        out.write(f"    JS_DECLARE_NATIVE_FUNCTION({idl_identifier_cpp_name(operation)});\n")
        if len(operations) > 1:
            for overload_index, overloaded_operation in enumerate(operations):
                out.write(
                    f"    JS_DECLARE_NATIVE_FUNCTION({idl_identifier_cpp_name(overloaded_operation, suffix=overload_index)});\n"
                )
    out.write(
        """};

"""
    )


def namespace_attributes(interface: Interface) -> list:
    attributes = [
        attribute for attribute in interface.regular_attributes if "FIXME" not in attribute.extended_attributes
    ]
    for attribute in attributes:
        # NB: Namespaces can only have read only regular attributes, see https://webidl.spec.whatwg.org/#idl-namespaces
        if not attribute.readonly:
            raise RuntimeError(f"Namespace attribute {interface.name}.{attribute.name} must be read only")
    return attributes


# https://webidl.spec.whatwg.org/#dfn-attribute-getter
def write_namespace_attribute_getters(
    out: TextIO, context: GenerationContext, includes: GeneratedIncludes, interface: Interface
) -> None:
    callee = fully_qualified_name_for_interface(interface).partition("::")[0]
    for attribute in namespace_attributes(interface):
        includes.add("LibWeb/WebIDL/ExceptionOrUtils.h")
        first_argument = "realm" if "NeedsCallerRealm" in attribute.extended_attributes else "vm"
        out.write(f"""JS_DEFINE_NATIVE_FUNCTION({interface.namespace_class}::{attribute_getter_callback_name(attribute)})
{{
    [[maybe_unused]] auto& realm = *vm.current_realm();

    // 1. Let idlObject be null.
    // NB: Step 2 only applies when target is an interface.

    // 3. Let R be the result of running the getter steps of attribute with idlObject as this.
    auto R = TRY(WebIDL::throw_dom_exception_if_needed(vm, realm, [&] {{ return {callee}::{idl_implementation_cpp_name(attribute)}({first_argument}); }}));

    // 4. Return the result of converting R to an ECMAScript value of the type attribute is declared as.
    return {to_javascript_value(attribute.type, "R", includes, context)};
}}

""")


# https://webidl.spec.whatwg.org/#namespace-object
def write_namespace_implementation(
    out: TextIO, context: GenerationContext, includes: GeneratedIncludes, interface: Interface
) -> None:
    includes.add("LibJS/Runtime/ValueInlines.h")
    includes.add_binding(interface.implemented_name)
    includes.add(implementation_header_for_interface(interface))

    # 1. Let namespaceObject be OrdinaryObjectCreate(realm.[[Intrinsics]].[[%Object.prototype%]]).
    # NB: The intrinsics create namespaceObject and pass it to initialize().
    out.write(
        f"""void {interface.namespace_class}::initialize(JS::Realm& realm, JS::Object& object)
{{
    [[maybe_unused]] auto& vm = realm.vm();
    [[maybe_unused]] u8 default_attributes = JS::Attribute::Writable | JS::Attribute::Enumerable | JS::Attribute::Configurable;

    // The class string of a namespace object is the namespace’s identifier.
    object.define_direct_property(vm.well_known_symbol_to_string_tag(), JS::PrimitiveString::create(vm, "{interface.name}"_utf16), JS::Attribute::Configurable);
"""
    )

    # 2. Define the regular attributes of namespace on namespaceObject given realm.
    define_the_regular_attributes(out, includes, interface)

    # 3. Define the regular operations of namespace on namespaceObject given realm.
    define_the_regular_operations(out, includes, interface)

    # 4. Define the constants of namespace on namespaceObject given realm.
    define_the_constants(out, context, includes, interface)

    # 5. For each exposed interface interface which has the [LegacyNamespace] extended attribute with the identifier of namespace as its argument,
    #     1. Let id be interface’s identifier.
    #     2. Let interfaceObject be the result of creating an interface object for interface with id in realm.
    #     3. Perform DefineMethodProperty(namespaceObject, id, interfaceObject, false).
    # 6. Return namespaceObject.
    # NB: Above is done in intrinsics defintions.

    if "WithInitializer" in interface.extended_attributes:
        out.write(
            f"""
    {fully_qualified_name_for_interface(interface).partition("::")[0]}::initialize(object, realm);
"""
        )
    out.write(
        """}

"""
    )
    write_namespace_attribute_getters(out, context, includes, interface)
    write_regular_operations(out, context, includes, interface)
