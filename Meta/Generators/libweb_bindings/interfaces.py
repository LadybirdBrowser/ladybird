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
from Generators.libweb_bindings.wrappers import has_legacy_override_built_ins_interface_extended_attribute
from Generators.libweb_bindings.wrappers import interface_needs_wrapper
from Generators.libweb_bindings.wrappers import needs_legacy_platform_object_flags_initialization
from Generators.libweb_bindings.wrappers import wrapper_base_class_name
from Generators.libweb_bindings.wrappers import wrapper_class_name
from Generators.libweb_bindings.wrappers import wrapper_needs_wrappable_impl
from Utils.webidl_parser import IDLType
from Utils.webidl_parser import Interface

GENERATED_GLOBAL_SCOPE_EXPOSURE_PREFIXES = {
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


def legacy_platform_object_flags_initialization(interface: Interface) -> str:
    if not needs_legacy_platform_object_flags_initialization(interface):
        return ""

    lines = [
        "    if (!m_legacy_platform_object_flags.has_value())",
        "        m_legacy_platform_object_flags = LegacyPlatformObjectFlags {};",
    ]
    if interface.indexed_property_getter is not None:
        lines.append("    m_legacy_platform_object_flags->supports_indexed_properties = true;")
    if interface.named_property_getter is not None:
        lines.append("    m_legacy_platform_object_flags->supports_named_properties = true;")
    if interface.indexed_property_setter is not None:
        lines.append("    m_legacy_platform_object_flags->has_indexed_property_setter = true;")
        if interface.indexed_property_setter.name:
            lines.append("    m_legacy_platform_object_flags->indexed_property_setter_has_identifier = true;")
    if interface.named_property_setter is not None:
        lines.append("    m_legacy_platform_object_flags->has_named_property_setter = true;")
        if interface.named_property_setter.name:
            lines.append("    m_legacy_platform_object_flags->named_property_setter_has_identifier = true;")
    if interface.named_property_deleter is not None:
        lines.append("    m_legacy_platform_object_flags->has_named_property_deleter = true;")
        if interface.named_property_deleter.name:
            lines.append("    m_legacy_platform_object_flags->named_property_deleter_has_identifier = true;")
    if "LegacyUnenumerableNamedProperties" in interface.extended_attributes:
        lines.append(
            "    m_legacy_platform_object_flags->has_legacy_unenumerable_named_properties_interface_extended_attribute = true;"
        )
    if has_legacy_override_built_ins_interface_extended_attribute(interface):
        lines.append(
            "    m_legacy_platform_object_flags->has_legacy_override_built_ins_interface_extended_attribute = true;"
        )
    if "Global" in interface.extended_attributes:
        lines.append("    m_legacy_platform_object_flags->has_global_interface_extended_attribute = true;")

    return "\n".join(lines)


def write_wrapper_implementation(
    out: TextIO, context: GenerationContext, includes: GeneratedIncludes, interface: Interface
) -> None:
    if not interface_needs_wrapper(interface):
        return

    wrapper_class = wrapper_class_name(interface)
    base_class = wrapper_base_class_name(context, interface)
    impl_type = fully_qualified_name_for_interface(interface)
    location_object_constructor_argument = ""
    if interface.name == "Location":
        location_object_constructor_argument = ", MayInterfereWithIndexedPropertyAccess::Yes"

    out.write(
        f"""GC_DEFINE_ALLOCATOR({wrapper_class});

"""
    )
    if interface.parent_name:
        out.write(
            f"""{wrapper_class}::{wrapper_class}(JS::Realm& realm, GC::Ref<{impl_type}> impl)
    : {base_class}(realm, impl)
{{
{legacy_platform_object_flags_initialization(interface)}
}}
"""
        )
    else:
        out.write(
            f"""{wrapper_class}::{wrapper_class}(JS::Realm& realm, GC::Ref<{impl_type}> impl)
    : {base_class}(realm{location_object_constructor_argument})
    , m_impl(impl)
{{
{legacy_platform_object_flags_initialization(interface)}
}}
"""
        )

    out.write(
        f"""
{wrapper_class}::~{wrapper_class}()
{{
"""
    )
    out.write(
        """}

"""
    )

    if interface.parent_name:
        out.write(
            f"""{impl_type}& {wrapper_class}::impl()
{{
    return static_cast<{impl_type}&>(Base::impl());
}}

{impl_type} const& {wrapper_class}::impl() const
{{
    return static_cast<{impl_type} const&>(Base::impl());
}}

"""
        )
    else:
        out.write(
            f"""{impl_type}& {wrapper_class}::impl()
{{
    return *m_impl;
}}

{impl_type} const& {wrapper_class}::impl() const
{{
    return *m_impl;
}}

"""
        )

    if wrapper_needs_wrappable_impl(context, interface):
        out.write(
            f"""Wrappable* {wrapper_class}::wrappable_impl()
{{
    return &impl();
}}

Wrappable const* {wrapper_class}::wrappable_impl() const
{{
    return &impl();
}}

"""
        )

    if interface.name == "DOMException":
        out.write(
            f"""JS::ErrorData* {wrapper_class}::error_data()
{{
    return &impl();
}}

JS::ErrorData const* {wrapper_class}::error_data() const
{{
    return &impl();
}}

"""
        )

    named_and_indexed_properties.write_legacy_platform_object_hook_implementations(out, context, includes, interface)
    named_and_indexed_properties.write_named_item_value_implementation(out, context, includes, interface)

    out.write(
        f"""void {wrapper_class}::initialize(JS::Realm& realm)
{{
"""
    )
    if "Global" in interface.extended_attributes:
        out.write(
            """    if (!realm.host_defined()) {
        PlatformObject::initialize(realm);
        return;
    }
"""
        )
    out.write(
        f"""    static auto const& name = *new FlyString("{interface.namespaced_name}"_fly_string);
    if (!shape().prototype())
        set_prototype(&ensure_web_prototype<{interface.prototype_class}>(realm, name));
    Base::initialize(realm);
"""
    )
    if "Global" not in interface.extended_attributes:
        out.write(
            f"""    {interface.prototype_class}::define_unforgeable_attributes(realm, *this);
"""
        )
    if interface.name == "Location":
        out.write(
            """    initialize_location_object(realm);
"""
        )
    out.write(
        """}

"""
    )
    if "Global" in interface.extended_attributes:
        out.write(
            f"""JS::ThrowCompletionOr<bool> {wrapper_class}::internal_set_prototype_of(JS::Object* prototype)
{{
    return set_immutable_prototype(prototype);
}}

"""
        )
    if interface.name == "HTMLAllCollection":
        out.write(
            f"""bool {wrapper_class}::is_htmldda() const
{{
    return true;
}}

"""
        )
    if not interface.parent_name:
        out.write(
            f"""void {wrapper_class}::visit_edges(JS::Cell::Visitor& visitor)
{{
    Base::visit_edges(visitor);
    visitor.visit(m_impl);
"""
        )
        if interface.name == "Location":
            out.write(
                """    visitor.visit(m_default_properties);
    visitor.visit(m_cross_origin_property_descriptor_map);
"""
            )
        out.write(
            """}

"""
        )


def write_impl_from(out: TextIO, includes: GeneratedIncludes, interface: Interface) -> None:
    if not interface_needs_impl_from(interface):
        return

    window_proxy_special_case = ""
    if interface.name in ("EventTarget", "Window"):
        window_proxy_special_case = """
    if (auto window_proxy = js_value.as_if<HTML::WindowProxy>())
        return window_proxy->window().ptr();
"""

    if interface_needs_wrapper(interface):
        includes.add("LibWeb/Bindings/Wrappable.h")

    out.write(
        f"""[[maybe_unused]] static JS::ThrowCompletionOr<{fully_qualified_name_for_interface(interface)}*> impl_from(JS::VM& vm, JS::Value js_value)
{{
{window_proxy_special_case}
    if (js_value.is_object()) {{
        if (auto* impl = Web::Bindings::impl_from<{fully_qualified_name_for_interface(interface)}>(&js_value.as_object()))
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
    includes.add("LibWeb/WebIDL/Types.h")
    includes.add("LibWeb/WebIDL/Tracing.h")
    includes.add_binding(interface.implemented_name)
    if interface_needs_wrapper(interface):
        includes.add("AK/StdLibExtras.h")
        includes.add("LibJS/Runtime/Realm.h")
        includes.add("LibWeb/Bindings/Wrappable.h")
        includes.add("LibWeb/Bindings/WrapperWorld.h")
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
            "DedicatedWorker": "add_dedicated_worker_exposed_interfaces",
            "SharedWorker": "add_shared_worker_exposed_interfaces",
        }[exposure_prefix]
        out.write(
            f"""}} // namespace Web::Bindings

namespace Web::HTML {{

void {interface.name}::initialize_web_interfaces_impl()
{{
    auto& realm = this->realm();
    auto& global_object = realm.global_object();

    Bindings::{add_exposed_interfaces_function}(global_object);

    Bindings::{interface.name}GlobalMixin global_mixin;
    global_mixin.initialize(realm, global_object);

    Base::initialize_web_interfaces_impl();
}}

}} // namespace Web::HTML

namespace Web::Bindings {{
"""
        )

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

        write_impl_from(out, includes, interface)
        operations.write_static_operations(out, context, includes, interface)
        attributes.write_static_attribute_getters(out, context, includes, interface)
        named_and_indexed_properties.write_named_properties_object_implementation(out, includes, interface)
        global_mixins.write_global_mixin_implementation(out, context, includes, interface)
        return
    attributes.define_the_regular_attributes(out, includes, interface)
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
    named_and_indexed_properties.write_named_properties_object_implementation(out, includes, interface)
    global_mixins.write_global_mixin_implementation(out, context, includes, interface)
