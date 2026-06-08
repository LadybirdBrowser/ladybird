# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause


from typing import Optional
from typing import TextIO

from Generators.libweb_bindings import attributes
from Generators.libweb_bindings import callback_interfaces
from Generators.libweb_bindings import constants
from Generators.libweb_bindings import constructors
from Generators.libweb_bindings import global_mixins
from Generators.libweb_bindings import interface_declaration
from Generators.libweb_bindings import iterables
from Generators.libweb_bindings import named_and_indexed_properties
from Generators.libweb_bindings import namespaces
from Generators.libweb_bindings import operations
from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.cpp_types import fully_qualified_name_for_interface
from Generators.libweb_bindings.cpp_types import implementation_header_for_interface
from Generators.libweb_bindings.includes import GeneratedIncludes
from Generators.libweb_bindings.interface_declaration import interface_requires_custom_prototype
from Generators.libweb_bindings.named_and_indexed_properties import interface_supports_named_properties
from Generators.libweb_bindings.overload_resolution import parameter_list_length
from Utils.webidl_parser import IDLType
from Utils.webidl_parser import Interface


def interface_needs_impl_from(interface: Interface) -> bool:
    return (
        bool(interface.regular_attributes)
        or bool(interface.regular_operations)
        or interface.stringifier is not None
        or interface.indexed_property_getter is not None
        or interface.named_property_getter is not None
        or interface.named_property_setter is not None
        or interface.named_property_deleter is not None
        or interface.maplike is not None
        or interface.setlike is not None
        or interface.iterable is not None
        or interface.async_iterable is not None
    )


def write_impl_from(out: TextIO, interface: Interface) -> None:
    if not interface_needs_impl_from(interface):
        return

    window_proxy_special_case = ""
    if interface.name in ("EventTarget", "Window"):
        window_proxy_special_case = """
    if (auto window_proxy = js_value.as_if<HTML::WindowProxy>())
        return window_proxy->window().ptr();
"""

    out.write(
        f"""[[maybe_unused]] static JS::ThrowCompletionOr<{fully_qualified_name_for_interface(interface)}*> impl_from(JS::VM& vm, JS::Value js_value)
{{
{window_proxy_special_case}
    if (auto impl = js_value.as_if<{fully_qualified_name_for_interface(interface)}>())
        return impl.ptr();
    return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "{interface.namespaced_name}");
}}

[[maybe_unused]] static JS::ThrowCompletionOr<{fully_qualified_name_for_interface(interface)}*> impl_from(JS::VM& vm)
{{
    auto this_value = vm.this_value();
    if (this_value.is_nullish())
        this_value = &vm.current_realm()->global_object();
    return impl_from(vm, this_value);
}}

"""
    )


def write_declaration(
    out: TextIO, includes: GeneratedIncludes, context: GenerationContext, interface: Optional[Interface]
) -> None:
    if interface is None:
        return

    interface_declaration.write_declaration(out, includes, context, interface)


def write_implementation(
    out: TextIO, includes: GeneratedIncludes, context: GenerationContext, interface: Optional[Interface]
) -> None:
    if interface is None:
        return

    if interface.is_callback_interface:
        callback_interfaces.write_callback_interface_implementation(out, context, includes, interface)
        return

    if interface.is_namespace:
        namespaces.write_namespace_implementation(out, context, includes, interface)
        return

    includes.add("LibJS/Runtime/ValueInlines.h")
    includes.add("LibWeb/Bindings/Intrinsics.h")
    includes.add("LibWeb/WebIDL/Tracing.h")
    includes.add_binding(interface.implemented_name)
    if interface.parent_name:
        parent_interface = context.interface(IDLType(interface.parent_name))
        includes.add_binding(parent_interface.implemented_name if parent_interface else interface.parent_name)
    includes.add(implementation_header_for_interface(interface))
    if interface_needs_impl_from(interface):
        includes.add("LibJS/Runtime/Error.h")
        includes.add("LibWeb/Bindings/ExceptionOrUtils.h")
    if interface.name in ("EventTarget", "Window") and interface_needs_impl_from(interface):
        includes.add("LibWeb/HTML/Window.h")
        includes.add("LibWeb/HTML/WindowProxy.h")
    if interface.constructors:
        includes.add("LibJS/Runtime/AbstractOperations.h")
        includes.add("LibJS/Runtime/Realm.h")
        includes.add("LibWeb/Bindings/ExceptionOrUtils.h")

    parent_prototype = "realm.intrinsics().object_prototype()"
    if interface.name == "DOMException":
        # https://webidl.spec.whatwg.org/#es-DOMException-specialness
        # Object.getPrototypeOf(DOMException.prototype) === Error.prototype
        parent_prototype = "realm.intrinsics().error_prototype()"
    if interface.parent_name:
        parent_prototype = f'GC::Ref {{ ensure_web_prototype<{interface.parent_name}Prototype>(realm, "{interface.parent_name}"_fly_string) }}'

    constructor_length = 0
    if interface.constructors:
        constructor_length = min(
            parameter_list_length(constructor.parameters) for constructor in interface.constructors
        )

    out.write(f"""void {interface.constructor_class}::initialize(JS::Realm& realm, JS::NativeFunction& object)
{{
    auto& vm = realm.vm();
    [[maybe_unused]] u8 default_attributes = JS::Attribute::Enumerable;

    {f'object.set_prototype(&ensure_web_constructor<{interface.parent_name}Prototype>(realm, "{interface.parent_name}"_fly_string));' if interface.parent_name else ""}
    object.define_direct_property(vm.names.length, JS::Value({constructor_length}), JS::Attribute::Configurable);
    object.define_direct_property(vm.names.name, JS::PrimitiveString::create(vm, "{interface.name}"_string), JS::Attribute::Configurable);
    object.define_direct_property(vm.names.prototype, &ensure_web_prototype<{interface.prototype_class}>(realm, "{interface.namespaced_name}"_fly_string), 0);
""")
    constants.define_the_constants(out, context, includes, interface)
    attributes.define_the_static_attributes(out, includes, interface)
    operations.define_the_static_operations(out, includes, interface)
    out.write(
        f"""}}

JS::ThrowCompletionOr<GC::Ref<JS::Object>> {interface.constructor_class}::construct([[maybe_unused]] InterfaceConstructor& constructor, [[maybe_unused]] JS::FunctionObject& new_target)
{{
    WebIDL::log_trace(constructor.vm(), "{interface.constructor_class}::construct");
"""
    )
    if interface.constructors:
        if len(interface.constructors) == 1:
            constructors.write_constructor_steps(out, context, includes, interface, interface.constructors[0])
        else:
            constructors.write_constructor_overload_arbiter(out, context, includes, interface)
    else:
        out.write(
            f'    return constructor.vm().throw_completion<JS::TypeError>(JS::ErrorType::NotAConstructor, "{interface.name}");\n'
        )
    out.write("}\n\n")

    if len(interface.constructors) > 1:
        for overload_index, constructor in enumerate(interface.constructors):
            constructors.write_constructor_function(out, context, includes, interface, constructor, overload_index)

    if interface_requires_custom_prototype(interface):
        out.write(
            f"""GC_DEFINE_ALLOCATOR({interface.prototype_class});

{interface.prototype_class}::{interface.prototype_class}([[maybe_unused]] JS::Realm& realm)
    : Object(ConstructWithPrototypeTag::Tag, {parent_prototype})
{{
}}

{interface.prototype_class}::~{interface.prototype_class}()
{{
}}

"""
        )
        if "Global" in interface.extended_attributes:
            out.write(f"""JS::ThrowCompletionOr<bool> {interface.prototype_class}::internal_set_prototype_of(JS::Object* prototype)
{{
    // 1. Return ? SetImmutablePrototype(O, V).
    return set_immutable_prototype(prototype);
}}

""")
        out.write(f"""
void {interface.prototype_class}::initialize(JS::Realm& realm)
{{
    auto& object = *this;
""")
    else:
        out.write(f"""void {interface.prototype_class}::initialize(JS::Realm& realm, JS::Object& object)
{{
""")
    out.write(
        f"""    [[maybe_unused]] auto& vm = realm.vm();
    [[maybe_unused]] u8 default_attributes = JS::Attribute::Enumerable | JS::Attribute::Configurable | JS::Attribute::Writable;

    object.set_prototype({parent_prototype});
"""
    )
    if interface_supports_named_properties(interface):
        includes.add("LibWeb/Bindings/Intrinsics.h")
        out.write(
            f'    object.set_prototype(&ensure_web_prototype<{interface.prototype_class}>(realm, "{interface.name}Properties"_fly_string));\n'
        )

    if "Global" in interface.extended_attributes:
        out.write(
            f'    object.define_direct_property(vm.well_known_symbol_to_string_tag(), JS::PrimitiveString::create(vm, "{interface.namespaced_name}"_string), JS::Attribute::Configurable);\n'
        )
        if interface_requires_custom_prototype(interface):
            out.write("    Base::initialize(realm);\n")
        out.write("}\n\n")

        write_impl_from(out, interface)
        operations.write_static_operations(out, context, includes, interface)
        attributes.write_static_attribute_getters(out, context, includes, interface)
        named_and_indexed_properties.write_named_properties_object_implementation(out, includes, interface)
        global_mixins.write_global_mixin_implementation(out, context, includes, interface)
        return
    attributes.define_the_regular_attributes(out, includes, interface)
    if interface.name == "CSSStyleProperties":
        includes.add("LibWeb/CSS/GeneratedCSSStyleProperties.h")
        out.write("    GeneratedCSSStyleProperties::initialize(realm, object);\n")
    operations.define_the_regular_operations(out, includes, interface)
    operations.define_the_stringifier(out, includes, interface)
    named_and_indexed_properties.define_the_indexed_property_getter(out, includes, interface)
    iterables.define_the_pair_iterable_declaration(out, includes, interface)
    iterables.define_the_async_iterable_declaration(out, interface)
    iterables.define_the_maplike_declaration(out, includes, interface)
    iterables.define_the_setlike_declaration(out, includes, interface)
    named_and_indexed_properties.define_the_named_property_getter(out, context, interface)
    named_and_indexed_properties.define_the_named_property_setter(out, context, interface)
    named_and_indexed_properties.define_the_named_property_deleter(out, context, interface)

    constants.define_the_constants(out, context, includes, interface)
    operations.define_unscopable_members(out, includes, interface)
    out.write(
        f'    object.define_direct_property(vm.well_known_symbol_to_string_tag(), JS::PrimitiveString::create(vm, "{interface.namespaced_name}"_string), JS::Attribute::Configurable);\n'
    )
    if interface_requires_custom_prototype(interface):
        out.write("    Base::initialize(realm);\n")

    out.write(f"""}}

void {interface.prototype_class}::define_unforgeable_attributes(JS::Realm& realm, [[maybe_unused]] JS::Object& object)
{{
    [[maybe_unused]] auto& vm = realm.vm();
    [[maybe_unused]] u8 default_attributes = JS::Attribute::Enumerable;
""")
    attributes.define_the_unforgeable_attributes(out, includes, interface)
    operations.define_the_regular_operations(out, includes, interface, unforgeable=True)
    operations.define_the_stringifier(out, includes, interface, unforgeable=True)
    out.write("}\n\n")

    write_impl_from(out, interface)
    operations.write_static_operations(out, context, includes, interface)
    attributes.write_static_attribute_getters(out, context, includes, interface)
    attributes.write_attribute_getters(out, context, includes, interface)
    attributes.write_attribute_setters(out, context, includes, interface)
    operations.write_regular_operations(out, context, includes, interface)
    operations.write_stringifier(out, context, includes, interface)
    named_and_indexed_properties.write_indexed_property_getter(out, context, includes, interface)
    iterables.write_pair_iterable_declaration_functions(out, context, includes, interface)
    iterables.write_iterator_prototype_implementation(out, includes, interface)
    iterables.write_async_iterable_declaration_functions(out, context, includes, interface)
    iterables.write_async_iterator_prototype_implementation(out, includes, interface)
    iterables.write_maplike_declaration_functions(out, context, includes, interface)
    iterables.write_setlike_declaration_functions(out, context, includes, interface)
    named_and_indexed_properties.write_named_property_getter(out, context, includes, interface)
    named_and_indexed_properties.write_named_property_setter(out, context, includes, interface)
    named_and_indexed_properties.write_named_property_deleter(out, context, includes, interface)
    named_and_indexed_properties.write_named_properties_object_implementation(out, includes, interface)
    global_mixins.write_global_mixin_implementation(out, context, includes, interface)
