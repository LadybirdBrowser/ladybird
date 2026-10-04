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
from Generators.libweb_bindings.realms import member_realm_expr
from Generators.libweb_bindings.to_idl_value import to_idl_value
from Generators.libweb_bindings.to_js_value import to_javascript_value
from Generators.libweb_bindings.wrappers import interface_and_inherited_interfaces
from Generators.libweb_bindings.wrappers import wrapper_class_name
from Utils.utils import title_case_to_snake_case
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


INDEXED_PROPERTY_SETTER_FUNCTIONS = (
    "set_value_of_new_indexed_property",
    "set_value_of_existing_indexed_property",
    "set_value_of_indexed_property",
)

NAMED_PROPERTY_SETTER_FUNCTIONS = (
    "set_value_of_new_named_property",
    "set_value_of_existing_named_property",
    "set_value_of_named_property",
)

# The functions of LegacyPlatformObjectInfo (LibWeb/Bindings/PlatformObject.h), in declaration order.
LEGACY_PLATFORM_OBJECT_INFO_FUNCTIONS = (
    ("item_value", "named_item_value")
    + INDEXED_PROPERTY_SETTER_FUNCTIONS
    + NAMED_PROPERTY_SETTER_FUNCTIONS
    + ("delete_value",)
)


def legacy_platform_object_function_name(interface: Interface, function: str) -> str:
    return f"{title_case_to_snake_case(wrapper_class_name(interface))}_{function}"


def legacy_platform_object_function_signature(interface: Interface, function: str) -> str:
    name = legacy_platform_object_function_name(interface, function)
    if function == "item_value":
        return f"Optional<JS::Value> {name}(JS::HostObject const& wrapper, [[maybe_unused]] WrapperWorld& wrapper_world, JS::Realm& realm, size_t index)"
    if function == "named_item_value":
        return f"JS::Value {name}(JS::HostObject const& wrapper, [[maybe_unused]] WrapperWorld& wrapper_world, JS::Realm& realm, Utf16FlyString const& name)"
    if function in INDEXED_PROPERTY_SETTER_FUNCTIONS:
        return (
            f"WebIDL::ExceptionOr<void> {name}(JS::HostObject& wrapper, JS::Realm& realm, u32 index, JS::Value value)"
        )
    if function in NAMED_PROPERTY_SETTER_FUNCTIONS:
        return f"WebIDL::ExceptionOr<void> {name}(JS::HostObject& wrapper, JS::Realm& realm, Utf16FlyString const& name, JS::Value value)"
    if function == "delete_value":
        return f"WebIDL::ExceptionOr<NamedPropertyDeletionResult> {name}(JS::HostObject& wrapper, Utf16FlyString const& name)"
    raise RuntimeError(f"Unknown legacy platform object function '{function}'")


def interface_declares_named_item_value(interface: Interface) -> bool:
    return (
        named_property_getter_call(interface) is not None
        or interface.name in SPECIAL_NAMED_PROPERTY_VALUE_INTERFACES
        or interface.name == "Document"
    )


def legacy_platform_object_functions_declared_by(interface: Interface) -> list[str]:
    functions = []
    if indexed_property_getter_call(interface) is not None:
        functions.append("item_value")
    if interface_declares_named_item_value(interface):
        functions.append("named_item_value")
    functions += indexed_property_setter_hook_names(interface)
    functions += named_property_setter_hook_names(interface)
    if interface.named_property_deleter is not None:
        functions.append("delete_value")
    return functions


# The functions of a wrapper's LegacyPlatformObjectInfo, mapped to the function of the nearest interface in its
# inheritance chain that declares them.
def legacy_platform_object_info_functions(context: GenerationContext, interface: Interface) -> dict[str, str]:
    functions: dict[str, str] = {}
    for interface_in_chain in interface_and_inherited_interfaces(context, interface):
        for function in legacy_platform_object_functions_declared_by(interface_in_chain):
            functions.setdefault(function, legacy_platform_object_function_name(interface_in_chain, function))
    return {
        function: functions[function] for function in LEGACY_PLATFORM_OBJECT_INFO_FUNCTIONS if function in functions
    }


def write_legacy_platform_object_function_declarations(out: TextIO, interface: Interface) -> None:
    functions = legacy_platform_object_functions_declared_by(interface)
    for function in functions:
        declaration = legacy_platform_object_function_signature(interface, function).replace("[[maybe_unused]] ", "")
        out.write(f"{declaration};\n")
    if functions:
        out.write("\n")


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
    method_name_by_interface = {
        "CSSNumericArray": "value_at",
        "CSSTransformValue": "component_at",
        "CSSUnparsedValue": "token_at",
        "CSSKeyframesRule": "item",
    }
    method_name = idl_implementation_cpp_name(operation) or method_name_by_interface.get(interface.name, "item")
    receiver = "const_cast<HTML::HTMLSelectElement&>(impl)" if interface.name == "HTMLSelectElement" else "impl"
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

    return f"impl.{method_name}(name)"


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
            "svg_list": """    if (index >= impl.items().size())
        return {};
    auto indexed_property_value = impl.items()[index];
""",
        }[value_mode]
        if value_mode == "svg_list":
            index_range_check = ""
            call = ""
        else:
            call = f"    auto R = {call};\n"
        out.write(
            f"""{legacy_platform_object_function_signature(interface, "item_value")}
{{
    [[maybe_unused]] auto& vm = realm.vm();
    auto const& impl = wrapped_implementation_of<{fully_qualified_name_for_interface(interface)} const>(wrapper);
{index_range_check}{call}{value_setup}    return {conversion};
}}

"""
        )

    write_legacy_platform_object_setter_implementations(out, context, includes, interface)
    write_legacy_platform_object_deleter_implementation(out, context, includes, interface)


def indexed_property_setter_call(interface: Interface, operation: SpecialOperation, hook_name: str) -> str:
    value_name = "idl_value"

    if operation.name:
        return f"impl.{idl_implementation_cpp_name(operation)}(index, {value_name})"
    if hook_name == "set_value_of_indexed_property":
        return f"impl.set_value_of_indexed_property(index, {value_name})"
    if interface.name in ("HTMLOptionsCollection", "HTMLSelectElement"):
        return f"impl.set_value_of_indexed_property(index, {value_name})"
    if interface.name in ("SVGLengthList", "SVGNumberList", "SVGTransformList"):
        return f"impl.replace_item({value_name}, index)"
    return f"impl.{hook_name}(index, {value_name})"


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
            "js_value",
            includes,
            context,
        )
        conversion_steps = (
            f"    auto js_value = value;\n    auto maybe_element = TRY(WebIDL::throw_dom_exception_if_needed(vm, realm, [&] {{ return {conversion}; }}));\n"
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
        conversion = to_idl_value(value_parameter, "js_value", includes, context)
        conversion_steps = f"    auto js_value = value;\n    auto {value_name} = TRY(WebIDL::throw_dom_exception_if_needed(vm, realm, [&] {{ return {conversion}; }}));\n"
    out.write(
        f"""{legacy_platform_object_function_signature(interface, hook_name)}
{{
    auto& vm = realm.vm();
    auto& impl = wrapped_implementation_of<{fully_qualified_name_for_interface(interface)}>(wrapper);
{conversion_steps}    TRY({call});
    return {{}};
}}

"""
    )


def named_property_setter_call(interface: Interface, operation: SpecialOperation, hook_name: str) -> str:
    value_name = "idl_value"

    if operation.name:
        return f"impl.{idl_implementation_cpp_name(operation)}(name, {value_name})"
    return f"impl.set_value_of_named_property(name, {value_name})"


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
    conversion = to_idl_value(value_parameter, "js_value", includes, context)
    call = named_property_setter_call(interface, operation, hook_name)

    out.write(
        f"""{legacy_platform_object_function_signature(interface, hook_name)}
{{
    auto& vm = realm.vm();
    auto& impl = wrapped_implementation_of<{fully_qualified_name_for_interface(interface)}>(wrapper);
    auto js_value = value;
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
    call = f"impl.{method_name}(name)"
    out.write(
        f"""{legacy_platform_object_function_signature(interface, "delete_value")}
{{
    auto& impl = wrapped_implementation_of<{fully_qualified_name_for_interface(interface)}>(wrapper);
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
    if interface.name == "Document":
        includes.add("LibWeb/DOM/BindingsGlue.h")
        out.write(
            f"""{legacy_platform_object_function_signature(interface, "named_item_value")}
{{
    return document_named_item_value(wrapper_world, realm, wrapped_implementation_of<DOM::Document const>(wrapper), name);
}}

"""
        )
        return

    if interface.name in SPECIAL_NAMED_PROPERTY_VALUE_INTERFACES:
        includes.add("AK/Variant.h")
        includes.add("LibWeb/DOM/Element.h")
        includes.add("LibWeb/DOM/HTMLCollection.h")
        includes.add("LibWeb/DOM/Node.h")
        includes.add("LibWeb/HTML/RadioNodeList.h")
        includes.add("LibWeb/Bindings/WrapperWorld.h")
        value_source_by_interface = {
            "HTMLAllCollection": "impl.named_item(name)",
            "HTMLFormControlsCollection": "impl.named_item_or_radio_node_list(name)",
            "HTMLFormElement": "impl.named_item_or_radio_node_list(name)",
        }
        out.write(
            f"""{legacy_platform_object_function_signature(interface, "named_item_value")}
{{
    auto const& impl = wrapped_implementation_of<{fully_qualified_name_for_interface(interface)} const>(wrapper);
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
        f"""{legacy_platform_object_function_signature(interface, "named_item_value")}
{{
    [[maybe_unused]] auto& vm = realm.vm();
    auto const& impl = wrapped_implementation_of<{fully_qualified_name_for_interface(interface)} const>(wrapper);
{value_setup}    return {conversion};
}}

"""
    )


def named_properties_object_name(interface: Interface) -> str:
    return f"{interface.name}Properties"


def named_properties_object_host_class_name(interface: Interface) -> str:
    return f"{title_case_to_snake_case(named_properties_object_name(interface))}_host_class"


def create_named_properties_object_function_name(interface: Interface) -> str:
    return f"create_{title_case_to_snake_case(named_properties_object_name(interface))}"


def write_named_properties_object_declaration(out: TextIO, includes: GeneratedIncludes, interface: Interface) -> None:
    includes.add("LibGC/Ptr.h")
    includes.add("LibJS/Forward.h")
    includes.add("LibJS/HostObjectABI.h")
    out.write(
        f"""// https://webidl.spec.whatwg.org/#named-properties-object
extern JSHostClass const {named_properties_object_host_class_name(interface)};
GC::Ref<JS::Object> {create_named_properties_object_function_name(interface)}(JS::Realm&);

"""
    )


def write_named_properties_object_implementation(
    out: TextIO,
    includes: GeneratedIncludes,
    interface: Interface,
) -> None:
    if not interface_supports_named_properties(interface):
        return

    # The named properties object finds the values of named properties through the Window that is its realm's global
    # object, and Window is the only [Global] interface with a named property getter.
    if interface.name != "Window":
        raise RuntimeError(f"{interface.name} would need a named properties object, which only Window has")

    includes.add("AK/TypeCasts.h")
    includes.add("AK/Utf16FlyString.h")
    includes.add("LibJS/HostClassBuilder.h")
    includes.add("LibJS/Runtime/HostObject.h")
    includes.add("LibJS/Runtime/PrimitiveString.h")
    includes.add("LibJS/Runtime/PropertyDescriptor.h")
    includes.add("LibJS/Runtime/PropertyKey.h")
    includes.add("LibWeb/Bindings/Intrinsics.h")
    includes.add("LibWeb/Bindings/PlatformObject.h")
    includes.add("LibWeb/Bindings/WrapperWorld.h")
    includes.add("LibWeb/Bindings/Wrappable.h")
    includes.add("LibWeb/HTML/Window.h")

    name = named_properties_object_name(interface)
    traits = f"{name}Traits"
    hooks = f"{title_case_to_snake_case(name)}_hooks"
    host_class = named_properties_object_host_class_name(interface)
    parent_prototype = (
        f'&ensure_web_prototype<{interface.parent_name}Prototype>(realm, "{interface.parent_name}"_utf16_fly_string)'
    )
    enumerable = "false" if "LegacyUnenumerableNamedProperties" in interface.extended_attributes else "true"

    out.write(
        f"""namespace {{

struct {traits} {{
    // https://webidl.spec.whatwg.org/#named-properties-object-getownproperty
    static JS::ThrowCompletionOr<Optional<JS::PropertyDescriptor>> get_own_property(JS::HostObject const& named_properties_object, JS::PropertyKey const& property_name)
    {{
        auto& realm = named_properties_object.shape().realm();
        auto& window_wrapper = as<JS::HostObject>(realm.global_object());
        auto* window = Web::Bindings::impl_from<HTML::Window>(&window_wrapper);
        VERIFY(window);

        if (TRY(is_named_property_exposed_on_object(window_wrapper, property_name))) {{
            auto property_name_string = Utf16FlyString {{ property_name.to_utf16_string() }};
            auto value = window_named_item_value(host_defined_wrapper_world(realm), realm, *window, property_name_string);
            return JS::PropertyDescriptor {{ .value = value, .writable = true, .enumerable = {enumerable}, .configurable = true }};
        }}

        return named_properties_object.ordinary_get_own_property(property_name);
    }}

    // https://webidl.spec.whatwg.org/#named-properties-object-defineownproperty
    static JS::ThrowCompletionOr<bool> define_own_property(JS::HostObject&, JS::PropertyKey const&, JS::PropertyDescriptor&, Optional<JS::PropertyDescriptor>*)
    {{
        return false;
    }}

    // https://webidl.spec.whatwg.org/#named-properties-object-delete
    static JS::ThrowCompletionOr<bool> delete_property(JS::HostObject&, JS::PropertyKey const&)
    {{
        return false;
    }}

    // https://webidl.spec.whatwg.org/#named-properties-object-preventextensions
    // NB: Failing keeps the named properties object extensible.
    static JS::ThrowCompletionOr<bool> prevent_extensions(JS::HostObject&)
    {{
        return false;
    }}
}};

}}

static constexpr JSHostObjectHooks {hooks} = JS::make_host_object_hooks<{traits}>();

// https://webidl.spec.whatwg.org/#named-properties-object-setprototypeof
// NB: Only a ShadowRealm's global prototype chain is mutable, so [[SetPrototypeOf]] is always SetImmutablePrototype, which
//     the class flag gives the object.
constexpr JSHostClass {host_class} = JS::make_host_class(JS_HOST_CLASS_OBJECT, "{name}"sv, nullptr, &{hooks}, nullptr,
    JS_HOST_CLASS_MAY_INTERFERE_WITH_INDEXED_PROPERTY_ACCESS
        | JS_HOST_CLASS_IMMUTABLE_PROTOTYPE
        | JS_HOST_CLASS_NOT_CACHEABLE_FOR_PROPERTY_ABSENCE
        | JS_HOST_CLASS_NOT_ELIGIBLE_FOR_OWN_PROPERTY_ENUMERATION_FAST_PATH);

GC::Ref<JS::Object> {create_named_properties_object_function_name(interface)}(JS::Realm& realm)
{{
    auto& vm = realm.vm();
    auto named_properties_object = JS::HostObject::create(realm, {host_class}, {parent_prototype});
    named_properties_object->define_direct_property(vm.well_known_symbol_to_string_tag(), JS::PrimitiveString::create(vm, "{name}"_utf16), JS::Attribute::Configurable);
    return named_properties_object;
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
    getter_realm_argument = member_realm_expr(operation, interface=interface)

    out.write(
        f"""JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::{idl_identifier_cpp_name(operation)})
{{
    auto& realm = *vm.current_realm();
    auto this_value = vm.this_value();
    [[maybe_unused]] auto* idl_object = TRY(impl_from(vm, this_value));
    auto& this_object_realm = this_value_realm(realm, this_value);

    if (vm.argument_count() < 1)
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::BadArgCountOne, "{operation.name}");

    JS::VM::TypeErrorRealmScope conversion_type_error_realm {{ vm, {getter_realm_argument} }};

"""
    )
    write_operation_parameter_conversions(out, operation.parameters, includes, context, getter_realm_argument)
    out.write(
        f"""
    conversion_type_error_realm.restore();

    auto R = TRY(WebIDL::throw_dom_exception_if_needed(vm, {getter_realm_argument}, [&] {{ return idl_object->{idl_implementation_cpp_name(operation)}({parameter_name}); }}));
    return {to_javascript_value(operation.return_type, "R", includes, context, getter_realm_argument)};
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
    getter_realm_argument = member_realm_expr(operation, interface=interface)

    out.write(
        f"""JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::{idl_identifier_cpp_name(operation)})
{{
    auto& realm = *vm.current_realm();
    auto this_value = vm.this_value();
    [[maybe_unused]] auto* idl_object = TRY(impl_from(vm, this_value));
    auto& this_object_realm = this_value_realm(realm, this_value);

    if (vm.argument_count() < 1)
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::BadArgCountOne, "{operation.name}");

    JS::VM::TypeErrorRealmScope conversion_type_error_realm {{ vm, {getter_realm_argument} }};

"""
    )
    write_operation_parameter_conversions(out, operation.parameters, includes, context, getter_realm_argument)
    out.write(
        f"""
    conversion_type_error_realm.restore();

    auto R = TRY(WebIDL::throw_dom_exception_if_needed(vm, {getter_realm_argument}, [&] {{ return idl_object->{idl_implementation_cpp_name(operation)}({parameter_name}); }}));
    return {to_javascript_value(operation.return_type, "R", includes, context, getter_realm_argument)};
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

    arguments = ", ".join(idl_identifier_cpp_name(parameter) for parameter in operation.parameters)
    operation_realm_argument = member_realm_expr(operation, interface=interface)

    out.write(
        f"""JS_DEFINE_NATIVE_FUNCTION({interface.prototype_class}::{idl_identifier_cpp_name(operation)})
{{
    auto& realm = *vm.current_realm();
    auto this_value = vm.this_value();
    [[maybe_unused]] auto* idl_object = TRY(impl_from(vm, this_value));
    [[maybe_unused]] auto& this_object_realm = this_value_realm(realm, this_value);
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

    out.write(
        f"""    JS::VM::TypeErrorRealmScope conversion_type_error_realm {{ vm, {operation_realm_argument} }};

"""
    )
    write_operation_parameter_conversions(out, operation.parameters, includes, context, operation_realm_argument)
    out.write(
        """    conversion_type_error_realm.restore();

"""
    )

    operation_returns_undefined = operation.return_type.name == "undefined"
    if "CEReactions" in operation.extended_attributes:
        ce_reactions_steps = wrap_with_ce_reactions(includes, "original_steps()", operation_realm_argument)
        out.write(
            f"""    auto original_steps = [&] {{
        return WebIDL::throw_dom_exception_if_needed(vm, {operation_realm_argument}, [&] {{ return idl_object->{idl_implementation_cpp_name(operation)}({arguments}); }});
    }};

    [[maybe_unused]] auto R = TRY({ce_reactions_steps});
    return {to_javascript_value(operation.return_type, "R", includes, context, operation_realm_argument)};
}}

"""
        )
        return

    return_statement = "return JS::js_undefined();"
    if not operation_returns_undefined:
        return_statement = (
            f"return {to_javascript_value(operation.return_type, 'R', includes, context, operation_realm_argument)};"
        )
    out.write(
        f"""    [[maybe_unused]] auto R = TRY(WebIDL::throw_dom_exception_if_needed(vm, {operation_realm_argument}, [&] {{ return idl_object->{idl_implementation_cpp_name(operation)}({arguments}); }}));
    {return_statement}
}}

"""
    )
