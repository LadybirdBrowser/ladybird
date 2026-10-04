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
from Generators.libweb_bindings.named_and_indexed_properties import interface_supports_named_properties
from Generators.libweb_bindings.named_and_indexed_properties import legacy_platform_object_info_functions
from Generators.libweb_bindings.overload_resolution import parameter_list_length
from Generators.libweb_bindings.wrappers import create_wrapper_function_name
from Generators.libweb_bindings.wrappers import interface_and_inherited_interfaces
from Generators.libweb_bindings.wrappers import interface_needs_wrapper
from Generators.libweb_bindings.wrappers import legacy_platform_object_info_fields
from Generators.libweb_bindings.wrappers import parent_interface
from Generators.libweb_bindings.wrappers import wrapper_host_class_flags
from Generators.libweb_bindings.wrappers import wrapper_host_class_hooks
from Generators.libweb_bindings.wrappers import wrapper_host_class_name
from Generators.libweb_bindings.wrappers import wrapper_legacy_platform_object_info_name
from Utils.webidl_parser import IDLType
from Utils.webidl_parser import Interface

GENERATED_GLOBAL_SCOPE_EXPOSURE_PREFIXES = {
    "AudioWorkletGlobalScope": "AudioWorklet",
    "DedicatedWorkerGlobalScope": "DedicatedWorker",
    "SharedWorkerGlobalScope": "SharedWorker",
}


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


def write_wrapper_host_class(
    out: TextIO, context: GenerationContext, includes: GeneratedIncludes, interface: Interface
) -> None:
    includes.add("LibJS/HostClassBuilder.h")
    includes.add("LibJS/HostObjectABI.h")

    legacy_platform_object_info = "nullptr"
    legacy_platform_object_info_fields_of_wrapper = legacy_platform_object_info_fields(context, interface)
    legacy_platform_object_info_functions_of_wrapper = legacy_platform_object_info_functions(context, interface)
    if legacy_platform_object_info_fields_of_wrapper is not None:
        legacy_platform_object_info_name = wrapper_legacy_platform_object_info_name(interface)
        legacy_platform_object_info = f"&{legacy_platform_object_info_name}"
        out.write(f"static constexpr LegacyPlatformObjectInfo {legacy_platform_object_info_name} {{\n")
        for field in legacy_platform_object_info_fields_of_wrapper:
            out.write(f"    .{field} = true,\n")
        for function, function_name in legacy_platform_object_info_functions_of_wrapper.items():
            out.write(f"    .{function} = {function_name},\n")
        out.write("};\n\n")
    elif legacy_platform_object_info_functions_of_wrapper:
        raise RuntimeError(f"Interface '{interface.name}' has special operations but is not a legacy platform object")

    parent = parent_interface(context, interface)
    parent_host_class = f"&{wrapper_host_class_name(parent)}" if parent is not None else "nullptr"
    hooks = wrapper_host_class_hooks(context, interface)
    flags = " | ".join(wrapper_host_class_flags(context, interface))
    out.write(
        f"""constexpr JSHostClass {wrapper_host_class_name(interface)} = JS::make_host_class(JS_HOST_CLASS_OBJECT, "{interface.name}"sv,
    {parent_host_class}, &{hooks}, {legacy_platform_object_info},
    {flags});

"""
    )


def write_create_wrapper_function(out: TextIO, context: GenerationContext, interface: Interface) -> None:
    impl_type = fully_qualified_name_for_interface(interface)
    host_class = wrapper_host_class_name(interface)
    out.write(
        f"""GC::Ref<JS::HostObject> {create_wrapper_function_name(interface)}(JS::Realm& realm, GC::Ref<{impl_type}> impl)
{{
"""
    )
    is_global = "Global" in interface.extended_attributes
    if is_global:
        # NB: The realm of a [Global] wrapper gets its host-defined data after the wrapper is created, and only then does
        #     setting up the realm's interfaces give the wrapper its prototype.
        out.write(
            f"""    if (!realm.host_defined())
        return JS::HostObject::create(realm, {host_class}, nullptr, impl);
"""
        )
    out.write(
        f"""    static auto const& name = "{interface.namespaced_name}"_utf16_fly_string;
    auto wrapper = JS::HostObject::create(realm, {host_class}, &ensure_web_prototype<{interface.prototype_class}>(realm, name), impl);
"""
    )
    # NB: The unforgeable attributes of a [Global] interface live on the global object, which its global mixin defines.
    for interface_in_chain in reversed(interface_and_inherited_interfaces(context, interface)):
        if "Global" in interface_in_chain.extended_attributes:
            continue
        out.write(f"    {interface_in_chain.prototype_class}::define_unforgeable_attributes(realm, *wrapper);\n")
    if interface.name == "Location":
        out.write("    initialize_location_object(realm, *wrapper);\n")
    out.write(
        """    return wrapper;
}

"""
    )


def write_wrapper_implementation(
    out: TextIO, context: GenerationContext, includes: GeneratedIncludes, interface: Interface
) -> None:
    if not interface_needs_wrapper(interface):
        return

    includes.add("LibJS/Runtime/HostObject.h")
    write_wrapper_host_class(out, context, includes, interface)
    named_and_indexed_properties.write_legacy_platform_object_hook_implementations(out, context, includes, interface)
    named_and_indexed_properties.write_named_item_value_implementation(out, context, includes, interface)
    write_create_wrapper_function(out, context, interface)


def write_impl_from(out: TextIO, includes: GeneratedIncludes, interface: Interface) -> None:
    if not interface_needs_impl_from(interface):
        return

    window_proxy_special_case = ""
    if interface.name in ("EventTarget", "Window"):
        window_proxy_special_case = """
    if (auto* window_proxy = js_value.is_object() ? HTML::WindowProxy::from_object(js_value.as_object()) : nullptr; window_proxy && window_proxy->window())
        return window_proxy->window().ptr();
"""

    if interface_needs_wrapper(interface):
        includes.add("LibWeb/Bindings/Wrappable.h")

    object_conversion = (
        f"Web::Bindings::impl_from<{fully_qualified_name_for_interface(interface)}>(&js_value.as_object())"
    )

    out.write(
        f"""[[maybe_unused]] static JS::ThrowCompletionOr<{fully_qualified_name_for_interface(interface)}*> impl_from(JS::VM& vm, JS::Value js_value)
{{
{window_proxy_special_case}
    if (js_value.is_object()) {{
        if (auto* impl = {object_conversion})
            return impl;
    }}
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
    includes.add("LibWebCommon/WebIDL/Types.h")
    includes.add_binding(interface.implemented_name)
    if interface_needs_wrapper(interface):
        includes.add("AK/StdLibExtras.h")
        includes.add("LibJS/Runtime/Realm.h")
        includes.add("LibWeb/Bindings/Wrappable.h")
        includes.add("LibWeb/Bindings/WrapperWorld.h")
    if interface.name == "CSSStyleProperties":
        includes.add("LibWeb/CSS/GeneratedCSSStyleProperties.h")
    if interface.parent_name:
        parent_interface = context.interface(IDLType(interface.parent_name))
        includes.add_binding(parent_interface.implemented_name if parent_interface else interface.parent_name)
    includes.add(implementation_header_for_interface(interface))
    if interface_needs_impl_from(interface):
        includes.add("LibJS/Runtime/Error.h")
        includes.add("LibWeb/WebIDL/ExceptionOrUtils.h")
    if interface.name in ("EventTarget", "Window") and interface_needs_impl_from(interface):
        includes.add("LibWeb/HTML/Window.h")
        includes.add("LibWeb/HTML/WindowProxy.h")
    if interface.constructors:
        includes.add("LibJS/Runtime/AbstractOperations.h")
        includes.add("LibJS/Runtime/Realm.h")
        includes.add("LibWeb/WebIDL/ExceptionOrUtils.h")
    if interface.name in GENERATED_GLOBAL_SCOPE_EXPOSURE_PREFIXES:
        exposure_prefix = GENERATED_GLOBAL_SCOPE_EXPOSURE_PREFIXES[interface.name]
        includes.add(f"LibWeb/Bindings/{exposure_prefix}ExposedInterfaces.h")
        includes.add_binding(f"{interface.name}GlobalMixin")

    write_wrapper_implementation(out, context, includes, interface)

    if interface.name in GENERATED_GLOBAL_SCOPE_EXPOSURE_PREFIXES:
        exposure_prefix = GENERATED_GLOBAL_SCOPE_EXPOSURE_PREFIXES[interface.name]
        add_exposed_interfaces_function = {
            "AudioWorklet": "add_audio_worklet_exposed_interfaces",
            "DedicatedWorker": "add_dedicated_worker_exposed_interfaces",
            "SharedWorker": "add_shared_worker_exposed_interfaces",
        }[exposure_prefix]
        implementation_namespace = fully_qualified_name_for_interface(interface).rsplit("::", 1)[0]
        out.write(
            f"""}} // namespace Web::Bindings

namespace Web::{implementation_namespace} {{

void {interface.name}::initialize_web_interfaces_impl()
{{
    auto& realm = this->realm();
    auto& global_object = realm.global_object();

    Bindings::{add_exposed_interfaces_function}(global_object);

    Bindings::{interface.name}GlobalMixin global_mixin;
    global_mixin.initialize(realm, global_object);

    Base::initialize_web_interfaces_impl();
}}

}} // namespace Web::{implementation_namespace}

namespace Web::Bindings {{
"""
        )

    parent_prototype = "realm.intrinsics().object_prototype()"
    if interface.name == "DOMException":
        # https://webidl.spec.whatwg.org/#es-DOMException-specialness
        # Object.getPrototypeOf(DOMException.prototype) === Error.prototype
        parent_prototype = "realm.intrinsics().error_prototype()"
    if interface.parent_name:
        parent_prototype = f'GC::Ref {{ ensure_web_prototype<{interface.parent_name}Prototype>(realm, "{interface.parent_name}"_utf16_fly_string) }}'

    constructor_length = 0
    if interface.constructors:
        constructor_length = min(
            parameter_list_length(constructor.parameters) for constructor in interface.constructors
        )

    out.write(f"""void {interface.constructor_class}::initialize(JS::Realm& realm, JS::NativeFunction& object)
{{
    auto& vm = realm.vm();
    [[maybe_unused]] u8 default_attributes = JS::Attribute::Enumerable;

    {f'object.set_prototype(&ensure_web_constructor<{interface.parent_name}Prototype>(realm, "{interface.parent_name}"_utf16_fly_string));' if interface.parent_name else ""}
    object.define_direct_property(vm.names.length, JS::Value({constructor_length}), JS::Attribute::Configurable);
    object.define_direct_property(vm.names.name, JS::PrimitiveString::create(vm, "{interface.name}"_utf16), JS::Attribute::Configurable);
    object.define_direct_property(vm.names.prototype, &ensure_web_prototype<{interface.prototype_class}>(realm, "{interface.namespaced_name}"_utf16_fly_string), 0);
""")
    constants.define_the_constants(out, context, includes, interface)
    attributes.define_the_static_attributes(out, includes, interface)
    operations.define_the_static_operations(out, includes, interface)
    out.write(
        f"""}}

JS::ThrowCompletionOr<GC::Ref<JS::Object>> {interface.constructor_class}::construct([[maybe_unused]] InterfaceConstructor& constructor, [[maybe_unused]] JS::FunctionObject& new_target)
{{
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
            f'    object.set_prototype(&ensure_web_prototype<{interface.prototype_class}>(realm, "{interface.name}Properties"_utf16_fly_string));\n'
        )

    if "Global" in interface.extended_attributes:
        out.write(
            f'    object.define_direct_property(vm.well_known_symbol_to_string_tag(), JS::PrimitiveString::create(vm, "{interface.namespaced_name}"_utf16), JS::Attribute::Configurable);\n'
        )
        out.write("}\n\n")

        write_impl_from(out, includes, interface)
        operations.write_static_operations(out, context, includes, interface)
        attributes.write_static_attribute_getters(out, context, includes, interface)
        named_and_indexed_properties.write_named_properties_object_implementation(out, includes, interface)
        global_mixins.write_global_mixin_implementation(out, context, includes, interface)
        return
    attributes.define_the_regular_attributes(out, includes, interface)
    if interface.name == "CSSStyleProperties":
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
        f'    object.define_direct_property(vm.well_known_symbol_to_string_tag(), JS::PrimitiveString::create(vm, "{interface.namespaced_name}"_utf16), JS::Attribute::Configurable);\n'
    )

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

    write_impl_from(out, includes, interface)
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
    global_mixins.write_global_mixin_implementation(out, context, includes, interface)
