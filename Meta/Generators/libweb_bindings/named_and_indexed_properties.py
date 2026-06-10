# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause


from typing import Optional
from typing import TextIO

from Generators.libweb_bindings.arguments import write_operation_parameter_conversions
from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.cpp_types import fully_qualified_name_for_interface
from Generators.libweb_bindings.cpp_types import idl_identifier_cpp_name
from Generators.libweb_bindings.cpp_types import idl_implementation_cpp_name
from Generators.libweb_bindings.cpp_types import implementation_header_for_interface
from Generators.libweb_bindings.extended_attributes import wrap_with_ce_reactions
from Generators.libweb_bindings.includes import GeneratedIncludes
from Generators.libweb_bindings.to_idl_value import to_idl_value
from Generators.libweb_bindings.to_js_value import to_javascript_value
from Utils.webidl_parser import IDLType
from Utils.webidl_parser import Interface
from Utils.webidl_parser import OperationParameter
from Utils.webidl_parser import SpecialOperation

SPECIAL_NAMED_PROPERTY_VALUE_INTERFACES = (
    "HTMLAllCollection",
    "HTMLFormControlsCollection",
    "HTMLFormElement",
)


def indexed_property_setter_hook_names(interface: Interface) -> tuple[str, ...]:
    if interface.indexed_property_setter is None:
        return ()
    if interface.indexed_property_setter.name:
        return ("set_value_of_indexed_property",)
    return ("set_value_of_new_indexed_property", "set_value_of_existing_indexed_property")


def named_property_setter_hook_names(interface: Interface) -> tuple[str, ...]:
    if interface.named_property_setter is None:
        return ()
    if interface.named_property_setter.name:
        return ("set_value_of_named_property",)
    return ("set_value_of_new_named_property", "set_value_of_existing_named_property")


def interface_supports_named_properties(interface: Interface) -> bool:
    return interface.named_property_getter is not None and "Global" in interface.extended_attributes


def write_legacy_platform_object_hook_declarations(out: TextIO, interface: Interface) -> None:
    if indexed_property_getter_call(interface) is not None:
        out.write("    virtual Optional<JS::Value> item_value(WrapperWorld&, JS::Realm&, size_t) const override;\n")
    if named_property_getter_call(interface) is not None or interface.name in SPECIAL_NAMED_PROPERTY_VALUE_INTERFACES:
        out.write(
            "    virtual JS::Value named_item_value(WrapperWorld&, JS::Realm&, FlyString const&) const override;\n"
        )
    for hook_name in indexed_property_setter_hook_names(interface):
        out.write(f"    virtual WebIDL::ExceptionOr<void> {hook_name}(JS::Realm&, u32, JS::Value) override;\n")
    for hook_name in named_property_setter_hook_names(interface):
        out.write(
            f"    virtual WebIDL::ExceptionOr<void> {hook_name}(JS::Realm&, String const&, JS::Value) override;\n"
        )
    if interface.named_property_deleter is not None:
        out.write(
            "    virtual WebIDL::ExceptionOr<NamedPropertyDeletionResult> delete_value(String const&) override;\n"
        )


def indexed_property_index_argument(operation: SpecialOperation) -> str:
    if len(operation.parameters) != 1:
        raise RuntimeError("Unsupported indexed property getter arity")

    parameter = operation.parameters[0]
    if parameter.type.name == "unsigned long":
        return "static_cast<u32>(index)"

    return "index"


def indexed_property_getter_call(interface: Interface) -> Optional[str]:
    operation = interface.indexed_property_getter
    if operation is None:
        return None

    index_argument = indexed_property_index_argument(operation)
    if interface.name == "CSSKeyframesRule":
        return None

    method_name_by_interface = {
        "CSSNumericArray": "value_at",
        "CSSTransformValue": "component_at",
        "CSSUnparsedValue": "token_at",
    }
    method_name = idl_implementation_cpp_name(operation) or method_name_by_interface.get(interface.name, "item")
    receiver = "const_cast<HTML::HTMLSelectElement&>(impl())" if interface.name == "HTMLSelectElement" else "impl()"
    return f"{receiver}.{method_name}({index_argument})"


def indexed_property_getter_value_mode(interface: Interface) -> str:
    if interface.name in ("CSSStyleDeclaration",):
        return "empty_string"
    if interface.name in ("CSSUnparsedValue", "DOMStringList", "DOMTokenList", "MediaList"):
        return "optional"
    if interface.name in ("SVGLengthList", "SVGNumberList", "SVGTransformList"):
        return "svg_list"
    if interface.name in ("SourceBufferList",):
        return "value"
    return "pointer"


def named_property_getter_call(interface: Interface) -> Optional[str]:
    operation = interface.named_property_getter
    if operation is None:
        return None

    if interface.name in (
        "Document",
        "HTMLAllCollection",
        "HTMLFormControlsCollection",
        "HTMLFormElement",
        "Window",
    ):
        return None

    method_name_by_interface = {
        "DOMStringMap": "determine_value_of_named_property",
        "Storage": "get_item",
    }
    method_name = method_name_by_interface.get(interface.name, idl_implementation_cpp_name(operation))
    if not method_name:
        return None

    argument = "String(name)" if interface.name == "Storage" else "name"
    return f"impl().{method_name}({argument})"


def named_property_getter_value_mode(interface: Interface) -> str:
    if interface.name in ("DOMStringMap",):
        return "value"
    if interface.name in ("Storage",):
        return "optional_undefined"
    if interface.name in ("MimeTypeArray", "Plugin", "PluginArray"):
        return "pointer_null"
    return "pointer_undefined"


def write_legacy_platform_object_hook_implementations(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
) -> None:
    operation = interface.indexed_property_getter
    call = indexed_property_getter_call(interface)
    if operation is not None and call is not None:
        if len(operation.parameters) != 1:
            raise RuntimeError(f"Unsupported indexed property getter arity on '{interface.name}'")

        includes.add("AK/Optional.h")
        includes.add("AK/NumericLimits.h")
        includes.add("LibJS/Runtime/Value.h")
        includes.add("LibWeb/Bindings/WrapperWorld.h")
        if interface.name == "HTMLOptionsCollection":
            includes.add("LibWeb/HTML/HTMLOptionElement.h")

        index_range_check = ""
        if operation.parameters[0].type.name == "unsigned long":
            index_range_check = """    if (index > NumericLimits<u32>::max())
        return {};

"""
        value_mode = indexed_property_getter_value_mode(interface)
        return_type = operation.return_type.clone_with_nullable(False)
        conversion_type = return_type
        if interface.name == "HTMLOptionsCollection":
            conversion_type = IDLType("Element")
        conversion = to_javascript_value(
            conversion_type, "indexed_property_value", includes, context, "realm", "wrapper_world"
        )
        value_setup = {
            "empty_string": """    if (R.is_empty())
        return {};
    auto& indexed_property_value = R;
""",
            "optional": """    if (!R.has_value())
        return {};
    auto& indexed_property_value = R.value();
""",
            "pointer": """    if (!R)
        return {};
    auto indexed_property_value = R;
""",
            "value": """    auto& indexed_property_value = R;
""",
            "svg_list": """    if (index >= impl().items().size())
        return {};
    auto indexed_property_value = impl().items()[index];
""",
        }[value_mode]
        if value_mode == "svg_list":
            index_range_check = ""
            call = ""
        else:
            call = f"    auto R = {call};\n"
        out.write(
            f"""Optional<JS::Value> {interface.name}Wrapper::item_value([[maybe_unused]] WrapperWorld& wrapper_world, JS::Realm& realm, size_t index) const
{{
    [[maybe_unused]] auto& vm = realm.vm();
{index_range_check}{call}{value_setup}    return {conversion};
}}

"""
        )

    write_legacy_platform_object_setter_implementations(out, context, includes, interface)
    write_legacy_platform_object_deleter_implementation(out, context, includes, interface)


def indexed_property_setter_call(interface: Interface, operation: SpecialOperation, hook_name: str) -> str:
    value_name = "idl_value"

    if operation.name:
        return f"impl().{idl_implementation_cpp_name(operation)}(index, {value_name})"
    if hook_name == "set_value_of_indexed_property":
        return f"impl().set_value_of_indexed_property(index, {value_name})"
    if interface.name in ("HTMLOptionsCollection", "HTMLSelectElement"):
        return f"impl().set_value_of_indexed_property(index, {value_name})"
    if interface.name in ("SVGLengthList", "SVGNumberList", "SVGTransformList"):
        return f"impl().replace_item({value_name}, index)"
    return f"impl().{hook_name}(index, {value_name})"


def svg_list_indexed_property_setter_conversion(
    context: GenerationContext,
    includes: GeneratedIncludes,
    value_parameter: OperationParameter,
) -> Optional[str]:
    if value_parameter.type.name not in ("SVGLength", "SVGNumber", "SVGTransform"):
        return None

    value_interface = context.interface(value_parameter.type)
    if value_interface is None:
        raise RuntimeError(f"Unknown SVG list item interface '{value_parameter.type.name}'")

    includes.add("LibJS/Runtime/Error.h")
    includes.add("LibJS/Runtime/Value.h")
    includes.add("LibJS/Runtime/ValueInlines.h")
    includes.add("LibWeb/Bindings/Wrappable.h")
    includes.add(implementation_header_for_interface(value_interface))

    value_type = fully_qualified_name_for_interface(value_interface)
    return f"""[&]() -> JS::ThrowCompletionOr<GC::Ref<{value_type}>> {{
        if (!value.is_object())
            return vm.throw_completion<JS::TypeError>("Value must be an {value_parameter.type.name}"sv);

        if (auto* impl = Web::Bindings::impl_from<{value_type}>(&value.as_object()))
            return GC::Ref {{ *impl }};
        return vm.throw_completion<JS::TypeError>("Value must be an {value_parameter.type.name}"sv);
    }}()"""


def write_indexed_property_setter_implementation(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
    hook_name: str,
) -> None:
    operation = interface.indexed_property_setter
    if operation is None:
        return
    if len(operation.parameters) != 2:
        raise RuntimeError(f"Unsupported indexed property setter arity on '{interface.name}'")

    includes.add("LibJS/Runtime/Value.h")
    includes.add("LibWeb/WebIDL/ExceptionOrUtils.h")
    value_parameter = operation.parameters[1]
    value_name = "idl_value"
    call = indexed_property_setter_call(interface, operation, hook_name)

    if interface.name in ("HTMLOptionsCollection", "HTMLSelectElement"):
        includes.add("LibWeb/DOM/Element.h")
        conversion = to_idl_value(
            OperationParameter(
                "idl_value", IDLType("Element", True), extended_attributes=value_parameter.extended_attributes
            ),
            "value",
            includes,
            context,
        )
        conversion_steps = (
            f"    auto maybe_element = TRY(WebIDL::throw_dom_exception_if_needed(vm, realm, [&] {{ return {conversion}; }}));\n"
            f"    Optional<GC::Ref<DOM::Element>> {value_name};\n"
            f"    if (maybe_element)\n"
            f"        {value_name} = GC::Ref {{ *maybe_element }};\n"
        )
    elif interface.name in ("SVGLengthList", "SVGNumberList", "SVGTransformList"):
        conversion = svg_list_indexed_property_setter_conversion(context, includes, value_parameter)
        if conversion is None:
            raise RuntimeError(f"Unsupported SVG list indexed property setter type on '{interface.name}'")
        conversion_steps = f"    auto {value_name} = TRY(WebIDL::throw_dom_exception_if_needed(vm, realm, [&] {{ return {conversion}; }}));\n"
    else:
        conversion = to_idl_value(value_parameter, "value", includes, context)
        conversion_steps = f"    auto {value_name} = TRY(WebIDL::throw_dom_exception_if_needed(vm, realm, [&] {{ return {conversion}; }}));\n"
    out.write(
        f"""WebIDL::ExceptionOr<void> {interface.name}Wrapper::{hook_name}(JS::Realm& realm, u32 index, JS::Value value)
{{
    auto& vm = realm.vm();
{conversion_steps}    TRY({call});
    return {{}};
}}

"""
    )


def named_property_setter_call(interface: Interface, operation: SpecialOperation, hook_name: str) -> str:
    value_name = "idl_value"

    if operation.name:
        return f"impl().{idl_implementation_cpp_name(operation)}(name, {value_name})"
    return f"impl().set_value_of_named_property(name, {value_name})"


def write_named_property_setter_implementation(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
    hook_name: str,
) -> None:
    operation = interface.named_property_setter
    if operation is None:
        return
    if len(operation.parameters) != 2:
        raise RuntimeError(f"Unsupported named property setter arity on '{interface.name}'")

    includes.add("LibJS/Runtime/Value.h")
    includes.add("LibWeb/WebIDL/ExceptionOrUtils.h")
    value_parameter = operation.parameters[1]
    value_name = "idl_value"
    conversion = to_idl_value(value_parameter, "value", includes, context)
    call = named_property_setter_call(interface, operation, hook_name)

    out.write(
        f"""WebIDL::ExceptionOr<void> {interface.name}Wrapper::{hook_name}(JS::Realm& realm, String const& name, JS::Value value)
{{
    auto& vm = realm.vm();
    auto {value_name} = TRY(WebIDL::throw_dom_exception_if_needed(vm, realm, [&] {{ return {conversion}; }}));
    TRY({call});
    return {{}};
}}

"""
    )


def write_legacy_platform_object_setter_implementations(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
) -> None:
    for hook_name in indexed_property_setter_hook_names(interface):
        write_indexed_property_setter_implementation(out, context, includes, interface, hook_name)

    for hook_name in named_property_setter_hook_names(interface):
        write_named_property_setter_implementation(out, context, includes, interface, hook_name)


def write_legacy_platform_object_deleter_implementation(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
) -> None:
    operation = interface.named_property_deleter
    if operation is None:
        return
    if len(operation.parameters) != 1:
        raise RuntimeError(f"Unsupported named property deleter arity on '{interface.name}'")

    result = "NamedPropertyDeletionResult::NotRelevant"
    if operation.return_type.name == "boolean":
        result = "delete_result ? NamedPropertyDeletionResult::DidNotFail : NamedPropertyDeletionResult::DidFail"
    elif not operation.name:
        result = "NamedPropertyDeletionResult::DidNotFail"

    method_name = idl_implementation_cpp_name(operation) if operation.name else "delete_named_property"
    call = f"impl().{method_name}(name)"
    out.write(
        f"""WebIDL::ExceptionOr<NamedPropertyDeletionResult> {interface.name}Wrapper::delete_value(String const& name)
{{
"""
    )
    if operation.return_type.name == "boolean":
        out.write(f"    auto delete_result = TRY({call});\n")
    else:
        out.write(f"    {call};\n")
    out.write(
        f"""    return {result};
}}

"""
    )


def write_named_item_value_implementation(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
) -> None:
    operation = interface.named_property_getter
    call = named_property_getter_call(interface)
    if interface.name in SPECIAL_NAMED_PROPERTY_VALUE_INTERFACES:
        includes.add("AK/Variant.h")
        includes.add("LibWeb/DOM/Element.h")
        includes.add("LibWeb/DOM/HTMLCollection.h")
        includes.add("LibWeb/DOM/Node.h")
        includes.add("LibWeb/HTML/RadioNodeList.h")
        includes.add("LibWeb/Bindings/WrapperWorld.h")
        value_source_by_interface = {
            "HTMLAllCollection": "impl().named_item(name)",
            "HTMLFormControlsCollection": "impl().named_item_or_radio_node_list(name)",
            "HTMLFormElement": "impl().named_item_or_radio_node_list(name)",
        }
        out.write(
            f"""JS::Value {interface.name}Wrapper::named_item_value(WrapperWorld& wrapper_world, JS::Realm& realm, FlyString const& name) const
{{
    return {value_source_by_interface[interface.name]}.visit(
        [](Empty) -> JS::Value {{ return JS::js_undefined(); }},
        [&wrapper_world, &realm](auto const& value) -> JS::Value {{ return wrap(wrapper_world, realm, value); }});
}}

"""
        )
        return

    if operation is None or call is None:
        return

    includes.add("LibJS/Runtime/Value.h")
    includes.add("LibWeb/Bindings/WrapperWorld.h")

    value_mode = named_property_getter_value_mode(interface)
    conversion_type = operation.return_type.clone_with_nullable(False)
    value_setup = {
        "value": """    auto named_property_value = {call};
""",
        "optional_undefined": """    auto named_property_value = {call};
    if (!named_property_value.has_value())
        return JS::js_undefined();
    auto& unwrapped_named_property_value = named_property_value.value();
""",
        "pointer_null": """    auto named_property_value = {call};
    if (!named_property_value)
        return JS::js_null();
""",
        "pointer_undefined": """    auto named_property_value = {call};
    if (!named_property_value)
        return JS::js_undefined();
""",
    }[value_mode].format(call=call)

    value_name = "unwrapped_named_property_value" if value_mode == "optional_undefined" else "named_property_value"
    conversion = to_javascript_value(conversion_type, value_name, includes, context, "realm", "wrapper_world")

    out.write(
        f"""JS::Value {interface.name}Wrapper::named_item_value([[maybe_unused]] WrapperWorld& wrapper_world, JS::Realm& realm, FlyString const& name) const
{{
    [[maybe_unused]] auto& vm = realm.vm();
{value_setup}    return {conversion};
}}

"""
    )


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
    if interface.name == "Window":
        includes.add("LibWeb/Bindings/PlatformObject.h")
        includes.add("LibWeb/Bindings/WrapperWorld.h")
        includes.add("LibWeb/Bindings/Wrappable.h")
        includes.add("LibWeb/HTML/Window.h")
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

    define_direct_property(vm.well_known_symbol_to_string_tag(), JS::PrimitiveString::create(vm, "{interface.name}Properties"_string), JS::Attribute::Configurable);

    set_prototype({parent_prototype});
}}

// https://webidl.spec.whatwg.org/#named-properties-object-getownproperty
JS::ThrowCompletionOr<Optional<JS::PropertyDescriptor>> {interface.name}Properties::internal_get_own_property(JS::PropertyKey const& property_name) const
{{
    auto& realm = this->realm();

"""
    )
    if interface.name == "Window":
        out.write(
            """    auto* object = as_if<PlatformObject>(&realm.global_object());
    VERIFY(object);
    auto* window = Web::Bindings::impl_from<HTML::Window>(&realm.global_object());
    VERIFY(window);
"""
        )
    else:
        out.write(
            f"""    using A = {fully_qualified_name_for_interface(interface)};
    auto* object = &as<A>(realm.global_object());
"""
        )
    out.write(
        """

    if (TRY(object->is_named_property_exposed_on_object(property_name))) {
        auto property_name_string = property_name.to_string().to_utf8_but_should_be_ported_to_utf16();

"""
    )
    if interface.name == "Window":
        out.write(
            "        auto value = window_named_item_value(host_defined_wrapper_world(realm), realm, *window, property_name_string);\n"
        )
    else:
        out.write("        auto value = object->named_item_value(realm, property_name_string);\n")
    out.write(
        f"""

        JS::PropertyDescriptor descriptor;

        descriptor.value = value;

        descriptor.enumerable = {"false" if "LegacyUnenumerableNamedProperties" in interface.extended_attributes else "true"};

        descriptor.writable = true;
        descriptor.configurable = true;

        return descriptor;
    }}

    return JS::Object::internal_get_own_property(property_name);
}}

// https://webidl.spec.whatwg.org/#named-properties-object-defineownproperty
JS::ThrowCompletionOr<bool> {interface.name}Properties::internal_define_own_property(JS::PropertyKey const&, JS::PropertyDescriptor&, Optional<JS::PropertyDescriptor>*)
{{
    return false;
}}

// https://webidl.spec.whatwg.org/#named-properties-object-delete
JS::ThrowCompletionOr<bool> {interface.name}Properties::internal_delete(JS::PropertyKey const&)
{{
    return false;
}}

// https://webidl.spec.whatwg.org/#named-properties-object-setprototypeof
JS::ThrowCompletionOr<bool> {interface.name}Properties::internal_set_prototype_of(JS::Object* prototype)
{{
    // NB: This is only ever true for ShadowRealms.

    return set_immutable_prototype(prototype);
}}

// https://webidl.spec.whatwg.org/#named-properties-object-preventextensions
JS::ThrowCompletionOr<bool> {interface.name}Properties::internal_prevent_extensions()
{{
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
    auto& realm = *vm.current_realm();
    [[maybe_unused]] auto* idl_object = TRY(impl_from(vm));

    if (vm.argument_count() < 1)
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::BadArgCountOne, "{operation.name}");

"""
    )
    write_operation_parameter_conversions(out, operation.parameters, includes, context)
    out.write(
        f"""

    auto R = TRY(WebIDL::throw_dom_exception_if_needed(vm, realm, [&] {{ return idl_object->{idl_implementation_cpp_name(operation)}({parameter_name}); }}));
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
    auto& realm = *vm.current_realm();
    [[maybe_unused]] auto* idl_object = TRY(impl_from(vm));

    if (vm.argument_count() < 1)
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::BadArgCountOne, "{operation.name}");

"""
    )
    write_operation_parameter_conversions(out, operation.parameters, includes, context)
    out.write(
        f"""

    auto R = TRY(WebIDL::throw_dom_exception_if_needed(vm, realm, [&] {{ return idl_object->{idl_implementation_cpp_name(operation)}({parameter_name}); }}));
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
        ce_reactions_steps = wrap_with_ce_reactions(includes, "original_steps()", "*vm.current_realm()")
        out.write(
            f"""    auto original_steps = [&] {{
        return WebIDL::throw_dom_exception_if_needed(vm, *vm.current_realm(), [&] {{ return idl_object->{idl_implementation_cpp_name(operation)}({arguments}); }});
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
        f"""    [[maybe_unused]] auto R = TRY(WebIDL::throw_dom_exception_if_needed(vm, *vm.current_realm(), [&] {{ return idl_object->{idl_implementation_cpp_name(operation)}({arguments}); }}));
    {return_statement}
}}

"""
    )
