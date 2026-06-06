# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause


from typing import TextIO

from Generators.libweb_bindings import overload_resolution
from Generators.libweb_bindings.attributes import define_the_regular_attributes
from Generators.libweb_bindings.constants import define_the_constants
from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.cpp_types import fully_qualified_name_for_interface
from Generators.libweb_bindings.cpp_types import idl_identifier_cpp_name
from Generators.libweb_bindings.cpp_types import implementation_header_for_interface
from Generators.libweb_bindings.includes import GeneratedIncludes
from Generators.libweb_bindings.operations import define_the_regular_operations
from Generators.libweb_bindings.operations import write_regular_operations
from Utils.webidl_parser import Interface


def write_namespace_declaration(
    out: TextIO, includes: GeneratedIncludes, context: GenerationContext, interface: Interface
) -> None:
    includes.add("LibJS/Runtime/NativeFunction.h")
    includes.add("LibJS/Runtime/Object.h")

    out.write(
        f"""class {interface.namespace_class} final : public JS::Object {{
    JS_OBJECT({interface.namespace_class}, JS::Object);
    GC_DECLARE_ALLOCATOR({interface.namespace_class});

public:
    explicit {interface.namespace_class}(JS::Realm&);
    virtual void initialize(JS::Realm&) override;
    virtual ~{interface.namespace_class}() override;

private:
"""
    )
    if "WithGCVisitor" in interface.extended_attributes:
        out.write("    virtual void visit_edges(JS::Cell::Visitor&) override;\n")
    if "WithFinalizer" in interface.extended_attributes:
        out.write(
            """
public:
    static constexpr bool OVERRIDES_FINALIZE = true;

private:
    virtual void finalize() override;
"""
        )
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


# https://webidl.spec.whatwg.org/#namespace-object
def write_namespace_implementation(
    out: TextIO, context: GenerationContext, includes: GeneratedIncludes, interface: Interface
) -> None:
    includes.add("LibJS/Runtime/ValueInlines.h")
    includes.add("LibWeb/WebIDL/Tracing.h")
    includes.add_binding(interface.implemented_name)
    includes.add(implementation_header_for_interface(interface))

    # 1. Let namespaceObject be OrdinaryObjectCreate(realm.[[Intrinsics]].[[%Object.prototype%]]).
    out.write(
        f"""GC_DEFINE_ALLOCATOR({interface.namespace_class});

{interface.namespace_class}::{interface.namespace_class}(JS::Realm& realm)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().object_prototype())
{{
}}

{interface.namespace_class}::~{interface.namespace_class}()
{{
}}

void {interface.namespace_class}::initialize(JS::Realm& realm)
{{
    auto& object = *this;
    [[maybe_unused]] auto& vm = this->vm();
    [[maybe_unused]] u8 default_attributes = JS::Attribute::Writable | JS::Attribute::Enumerable | JS::Attribute::Configurable;

    Base::initialize(realm);

    // The class string of a namespace object is the namespace’s identifier.
    define_direct_property(vm.well_known_symbol_to_string_tag(), JS::PrimitiveString::create(vm, "{interface.name}"_string), JS::Attribute::Configurable);
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
    {fully_qualified_name_for_interface(interface).partition("::")[0]}::initialize(*this, realm);
"""
        )
    out.write(
        """}

"""
    )
    write_regular_operations(out, context, includes, interface)

    if "WithGCVisitor" in interface.extended_attributes:
        out.write(
            f"""void {interface.namespace_class}::visit_edges(JS::Cell::Visitor& visitor)
{{
    Base::visit_edges(visitor);
    {fully_qualified_name_for_interface(interface).partition("::")[0]}::visit_edges(*this, visitor);
}}

"""
        )
    if "WithFinalizer" in interface.extended_attributes:
        out.write(
            f"""void {interface.namespace_class}::finalize()
{{
    Base::finalize();
    {fully_qualified_name_for_interface(interface).partition("::")[0]}::finalize(*this);
}}

"""
        )
