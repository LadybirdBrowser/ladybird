# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause


from typing import TextIO

from Generators.libweb_bindings.arguments import write_operation_parameter_conversions
from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.cpp_types import fully_qualified_name_for_interface
from Generators.libweb_bindings.cpp_types import idl_identifier_cpp_name
from Generators.libweb_bindings.cpp_types import idl_implementation_cpp_name
from Generators.libweb_bindings.cpp_types import implementation_header_for_interface
from Generators.libweb_bindings.extended_attributes import wrap_with_ce_reactions
from Generators.libweb_bindings.includes import GeneratedIncludes
from Generators.libweb_bindings.to_js_value import to_javascript_value
from Utils.webidl_parser import Interface
from Utils.webidl_parser import SpecialOperation


def interface_supports_named_properties(interface: Interface) -> bool:
    return interface.named_property_getter is not None and "Global" in interface.extended_attributes


def write_named_properties_object_declaration(out: TextIO, includes: GeneratedIncludes, interface: Interface) -> None:
    includes.add("AK/Optional.h")
    includes.add("LibGC/Ptr.h")
    includes.add("LibJS/Runtime/Object.h")
    includes.add("LibJS/Runtime/PropertyDescriptor.h")
    includes.add("LibJS/Runtime/PropertyKey.h")
    out.write(
        f"""class {interface.name}Properties : public JS::Object {{
    JS_OBJECT({interface.name}Properties, JS::Object);
    GC_DECLARE_ALLOCATOR({interface.name}Properties);

public:
    explicit {interface.name}Properties(JS::Realm&);
    virtual void initialize(JS::Realm&) override;
    virtual ~{interface.name}Properties() override;

    JS::Realm& realm() const {{ return m_realm; }}

private:
    virtual JS::ThrowCompletionOr<Optional<JS::PropertyDescriptor>> internal_get_own_property(JS::PropertyKey const&) const override;
    virtual JS::ThrowCompletionOr<bool> internal_define_own_property(JS::PropertyKey const&, JS::PropertyDescriptor&, Optional<JS::PropertyDescriptor>* precomputed_get_own_property = nullptr) override;
    virtual JS::ThrowCompletionOr<bool> internal_delete(JS::PropertyKey const&) override;
    virtual JS::ThrowCompletionOr<bool> internal_set_prototype_of(JS::Object* prototype) override;
    virtual JS::ThrowCompletionOr<bool> internal_prevent_extensions() override;

    virtual bool eligible_for_own_property_enumeration_fast_path() const override final {{ return false; }}

    virtual void visit_edges(Visitor&) override;

    GC::Ref<JS::Realm> m_realm;
}};

"""
    )


def write_named_properties_object_implementation(
    out: TextIO,
    includes: GeneratedIncludes,
    interface: Interface,
) -> None:
    if not interface_supports_named_properties(interface):
        return

    includes.add("AK/TypeCasts.h")
    includes.add("LibJS/Runtime/PrimitiveString.h")
    includes.add("LibJS/Runtime/PropertyDescriptor.h")
    includes.add("LibJS/Runtime/PropertyKey.h")
    includes.add("LibWeb/Bindings/Intrinsics.h")
    includes.add(implementation_header_for_interface(interface))
    parent_prototype = "realm.intrinsics().object_prototype()"
    if interface.parent_name:
        parent_prototype = (
            f'&ensure_web_prototype<{interface.parent_name}Prototype>(realm, "{interface.parent_name}"_fly_string)'
        )
    out.write(
        f"""GC_DEFINE_ALLOCATOR({interface.name}Properties);

{interface.name}Properties::{interface.name}Properties(JS::Realm& realm)
    : JS::Object(realm, nullptr, MayInterfereWithIndexedPropertyAccess::Yes)
    , m_realm(realm)
{{
}}

{interface.name}Properties::~{interface.name}Properties()
{{
}}

void {interface.name}Properties::initialize(JS::Realm& realm)
{{
    Base::initialize(realm);
    auto& vm = realm.vm();

    // The class string of a named properties object is the concatenation of the interface's identifier and the string "Properties".
    define_direct_property(vm.well_known_symbol_to_string_tag(), JS::PrimitiveString::create(vm, "{interface.name}Properties"_utf16), JS::Attribute::Configurable);

    // 1. Let proto be null.
    // 2. If interface is declared to inherit from another interface, then set proto to the interface prototype object in realm for the inherited interface.
    // 3. Otherwise, set proto to realm.[[Intrinsics]].[[%Object.prototype%]].
    // 10. Set obj.[[Prototype]] to proto.
    set_prototype({parent_prototype});
}}

// https://webidl.spec.whatwg.org/#named-properties-object-getownproperty
JS::ThrowCompletionOr<Optional<JS::PropertyDescriptor>> {interface.name}Properties::internal_get_own_property(JS::PropertyKey const& property_name) const
{{
    auto& realm = this->realm();

    // 1. Let A be the interface for the named properties object O.
    using A = {fully_qualified_name_for_interface(interface)};

    // 2. Let object be O.[[Realm]]'s global object.
    // 3. Assert: object implements A.
    auto& object = as<A>(realm.global_object());

    // 4. If the result of running the named property visibility algorithm with property name P and object object is true, then:
    if (TRY(object.is_named_property_exposed_on_object(property_name))) {{
        auto property_name_string = property_name.to_utf16_string().to_utf8_but_should_be_ported_to_utf16();

        // 1. Let operation be the operation used to declare the named property getter.
        // 2. Let value be an uninitialized variable.
        // 3. If operation was defined without an identifier, then set value to the result of performing the steps listed in the interface description to determine the value of a named property with P as the name.
        // 4. Otherwise, operation was defined with an identifier. Set value to the result of performing the method steps of operation with « P » as the only argument value.
        auto value = object.named_item_value(property_name_string);

        // 5. Let desc be a newly created Property Descriptor with no fields.
        JS::PropertyDescriptor descriptor;

        // 6. Set desc.[[Value]] to the result of converting value to an ECMAScript value.
        descriptor.value = value;

        // 7. If A implements an interface with the [LegacyUnenumerableNamedProperties] extended attribute, then set desc.[[Enumerable]] to false, otherwise set it to true.
        descriptor.enumerable = {"false" if "LegacyUnenumerableNamedProperties" in interface.extended_attributes else "true"};

        // 8. Set desc.[[Writable]] to true and desc.[[Configurable]] to true.
        descriptor.writable = true;
        descriptor.configurable = true;

        // 9. Return desc.
        return descriptor;
    }}

    // 5. Return OrdinaryGetOwnProperty(O, P).
    return JS::Object::internal_get_own_property(property_name);
}}

// https://webidl.spec.whatwg.org/#named-properties-object-defineownproperty
JS::ThrowCompletionOr<bool> {interface.name}Properties::internal_define_own_property(JS::PropertyKey const&, JS::PropertyDescriptor&, Optional<JS::PropertyDescriptor>*)
{{
    // 1. Return false.
    return false;
}}

// https://webidl.spec.whatwg.org/#named-properties-object-delete
JS::ThrowCompletionOr<bool> {interface.name}Properties::internal_delete(JS::PropertyKey const&)
{{
    // 1. Return false.
    return false;
}}

// https://webidl.spec.whatwg.org/#named-properties-object-setprototypeof
JS::ThrowCompletionOr<bool> {interface.name}Properties::internal_set_prototype_of(JS::Object* prototype)
{{
    // 1. If O’s associated realm’s is global prototype chain mutable is true, return ? OrdinarySetPrototypeOf(O, V).
    // NB: This is only ever true for ShadowRealms.

    // 2. Return ? SetImmutablePrototype(O, V).
    return set_immutable_prototype(prototype);
}}

// https://webidl.spec.whatwg.org/#named-properties-object-preventextensions
JS::ThrowCompletionOr<bool> {interface.name}Properties::internal_prevent_extensions()
{{
    // 1. Return false.
    // Note: this keeps named properties object extensible by making [[PreventExtensions]] fail.
    return false;
}}

void {interface.name}Properties::visit_edges(Visitor& visitor)
{{
    Base::visit_edges(visitor);
    visitor.visit(m_realm);
}}

"""
    )


def define_the_indexed_property_getter(
    out: TextIO,
    includes: GeneratedIncludes,
    interface: Interface,
) -> None:
    if interface.indexed_property_getter is None:
        return

    operation = interface.indexed_property_getter

    includes.add("LibJS/Runtime/ArrayPrototype.h")
    if operation.name:
        out.write(
            f"""    object.define_native_function(realm, "{operation.name}"_utf16_fly_string, {idl_identifier_cpp_name(operation)}, {len(operation.parameters)}, default_attributes);
"""
        )

    if interface.named_property_getter is not None and interface.named_property_getter.name:
        operation = interface.named_property_getter
        out.write(
            f"""    object.define_native_function(realm, "{operation.name}"_utf16_fly_string, {idl_identifier_cpp_name(operation)}, {len(operation.parameters)}, default_attributes);
"""
        )

    out.write(
        """    object.define_direct_property(vm.well_known_symbol_iterator(), realm.intrinsics().array_prototype()->get_without_side_effects(vm.names.values), JS::Attribute::Configurable | JS::Attribute::Writable);

"""
    )

    if interface.iterable is not None and interface.iterable.key_type is None:
        out.write(
            """    object.define_direct_property(vm.names.entries, realm.intrinsics().array_prototype()->get_without_side_effects(vm.names.entries), default_attributes);
    object.define_direct_property(vm.names.keys, realm.intrinsics().array_prototype()->get_without_side_effects(vm.names.keys), default_attributes);
    object.define_direct_property(vm.names.values, realm.intrinsics().array_prototype()->get_without_side_effects(vm.names.values), default_attributes);
    object.define_direct_property(vm.names.forEach, realm.intrinsics().array_prototype()->get_without_side_effects(vm.names.forEach), default_attributes);

"""
        )


def define_the_named_property_getter(out: TextIO, context: GenerationContext, interface: Interface) -> None:
    if interface.named_property_getter is None:
        return
    if interface.indexed_property_getter is not None:
        return

    operation = interface.named_property_getter
    if not operation.name:
        return

    out.write(
        f"""    object.define_native_function(realm, "{operation.name}"_utf16_fly_string, {idl_identifier_cpp_name(operation)}, {len(operation.parameters)}, default_attributes);

"""
    )


def define_the_named_property_setter(out: TextIO, context: GenerationContext, interface: Interface) -> None:
    if interface.named_property_setter is None:
        return

    operation = interface.named_property_setter
    if not operation.name:
        return

    out.write(
        f"""    object.define_native_function(realm, "{operation.name}"_utf16_fly_string, {idl_identifier_cpp_name(operation)}, {len(operation.parameters)}, default_attributes);

"""
    )


def define_the_named_property_deleter(out: TextIO, context: GenerationContext, interface: Interface) -> None:
    if interface.named_property_deleter is None:
        return

    operation = interface.named_property_deleter
    if not operation.name:
        return

    out.write(
        f"""    object.define_native_function(realm, "{operation.name}"_utf16_fly_string, {idl_identifier_cpp_name(operation)}, {len(operation.parameters)}, default_attributes);

"""
    )


def write_indexed_property_getter(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
) -> None:
    if interface.indexed_property_getter is None:
        return

    operation = interface.indexed_property_getter
    if not operation.name:
        return

    if len(operation.parameters) != 1:
        raise RuntimeError(f"Unsupported indexed property getter arity on '{interface.name}'")

    parameter = operation.parameters[0]
    parameter_name = idl_identifier_cpp_name(parameter)

    out.write(
        f"""JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::{idl_identifier_cpp_name(operation)})
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::{idl_identifier_cpp_name(operation)}");
    [[maybe_unused]] auto* idl_object = TRY(impl_from(vm));

    if (vm.argument_count() < 1)
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::BadArgCountOne, "{operation.name}");

"""
    )
    write_operation_parameter_conversions(out, operation.parameters, includes, context)
    out.write(
        f"""

    auto R = TRY(throw_dom_exception_if_needed(vm, [&] {{ return idl_object->{idl_implementation_cpp_name(operation)}({parameter_name}); }}));
    return {to_javascript_value(operation.return_type, "R", includes, context)};
}}

"""
    )


def write_named_property_getter(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
) -> None:
    if interface.named_property_getter is None:
        return

    operation = interface.named_property_getter
    if not operation.name:
        return

    if len(operation.parameters) != 1:
        raise RuntimeError(f"Unsupported named property getter arity on '{interface.name}'")

    parameter = operation.parameters[0]
    parameter_name = idl_identifier_cpp_name(parameter)

    out.write(
        f"""JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::{idl_identifier_cpp_name(operation)})
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::{idl_identifier_cpp_name(operation)}");
    [[maybe_unused]] auto* idl_object = TRY(impl_from(vm));

    if (vm.argument_count() < 1)
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::BadArgCountOne, "{operation.name}");

"""
    )
    write_operation_parameter_conversions(out, operation.parameters, includes, context)
    out.write(
        f"""

    auto R = TRY(throw_dom_exception_if_needed(vm, [&] {{ return idl_object->{idl_implementation_cpp_name(operation)}({parameter_name}); }}));
    return {to_javascript_value(operation.return_type, "R", includes, context)};
}}

"""
    )


def write_named_property_setter(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
) -> None:
    if interface.named_property_setter is None:
        return

    operation = interface.named_property_setter
    if not operation.name:
        return

    write_named_property_operation(out, context, includes, interface, operation)


def write_named_property_deleter(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
) -> None:
    if interface.named_property_deleter is None:
        return

    operation = interface.named_property_deleter
    if not operation.name:
        return

    write_named_property_operation(out, context, includes, interface, operation)


def write_named_property_operation(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
    operation: SpecialOperation,
) -> None:
    if not operation.parameters:
        raise RuntimeError(f"Unsupported named property operation arity on '{interface.name}'")

    return_value = to_javascript_value(operation.return_type, "R", includes, context)
    arguments = ", ".join(idl_identifier_cpp_name(parameter) for parameter in operation.parameters)

    out.write(
        f"""JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::{idl_identifier_cpp_name(operation)})
{{
    WebIDL::log_trace(vm, "{interface.prototype_class}::{idl_identifier_cpp_name(operation)}");
    [[maybe_unused]] auto* idl_object = TRY(impl_from(vm));
"""
    )
    if len(operation.parameters) == 1:
        out.write(
            f"""    if (vm.argument_count() < 1)
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::BadArgCountOne, "{operation.name}");

"""
        )
    else:
        out.write(
            f"""    if (vm.argument_count() < {len(operation.parameters)})
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::BadArgCountMany, "{operation.name}", "{len(operation.parameters)}");

"""
        )

    write_operation_parameter_conversions(out, operation.parameters, includes, context)

    operation_returns_undefined = operation.return_type.name == "undefined"
    if "CEReactions" in operation.extended_attributes:
        ce_reactions_steps = wrap_with_ce_reactions(includes, "original_steps()")
        out.write(
            f"""    auto original_steps = [&] {{
        return throw_dom_exception_if_needed(vm, [&] {{ return idl_object->{idl_implementation_cpp_name(operation)}({arguments}); }});
    }};

    [[maybe_unused]] auto R = TRY({ce_reactions_steps});
    return {return_value};
}}

"""
        )
        return

    return_statement = "return JS::js_undefined();"
    if not operation_returns_undefined:
        return_statement = f"return {return_value};"
    out.write(
        f"""    [[maybe_unused]] auto R = TRY(throw_dom_exception_if_needed(vm, [&] {{ return idl_object->{idl_implementation_cpp_name(operation)}({arguments}); }}));
    {return_statement}
}}

"""
    )
