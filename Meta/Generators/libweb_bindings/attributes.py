# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from __future__ import annotations

from io import StringIO
from typing import TextIO

from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.cpp_types import cpp_type_for_idl_type_details
from Generators.libweb_bindings.cpp_types import fully_qualified_name_for_interface
from Generators.libweb_bindings.cpp_types import idl_identifier_cpp_name
from Generators.libweb_bindings.cpp_types import idl_implementation_cpp_name
from Generators.libweb_bindings.extended_attributes import wrap_with_ce_reactions
from Generators.libweb_bindings.extended_attributes import wrap_with_extended_attribute_exposure_checks
from Generators.libweb_bindings.glue_headers import bindings_glue_header_for_interface
from Generators.libweb_bindings.includes import GeneratedIncludes
from Generators.libweb_bindings.realms import member_realm_expr
from Generators.libweb_bindings.to_idl_value import to_idl_value
from Generators.libweb_bindings.to_js_value import to_javascript_value
from Utils.webidl_parser import Attribute
from Utils.webidl_parser import IDLParameterizedType
from Utils.webidl_parser import IDLType
from Utils.webidl_parser import Interface


def attribute_has_setter(attribute: Attribute) -> bool:
    return (
        not attribute.readonly
        or "LegacyLenientSetter" in attribute.extended_attributes
        or "PutForwards" in attribute.extended_attributes
        or "Replaceable" in attribute.extended_attributes
    )


def attribute_is_nullable_reflected_frozen_array_of_element(attribute: Attribute) -> bool:
    return (
        "Reflect" in attribute.extended_attributes
        and attribute.type.nullable
        and isinstance(attribute.type, IDLParameterizedType)
        and attribute.type.name == "FrozenArray"
        and len(attribute.type.parameters) == 1
        and attribute.type.parameters[0].name == "Element"
    )


def attribute_is_nullable_reflected_element(attribute: Attribute) -> bool:
    return "Reflect" in attribute.extended_attributes and attribute.type.nullable and attribute.type.name == "Element"


def attribute_uses_cached_js_value(attribute: Attribute) -> bool:
    return (
        "CachedAttribute" in attribute.extended_attributes and "LegacyUnforgeable" not in attribute.extended_attributes
    ) or attribute_is_nullable_reflected_frozen_array_of_element(attribute)


def validate_direct_getter(context: GenerationContext, interface: Interface, attribute: Attribute) -> None:
    if "DirectGetter" not in attribute.extended_attributes:
        return

    description = f"[DirectGetter] attribute '{attribute.name}' on '{interface.name}'"
    resolved_type = context.resolve_typedef(attribute.type)
    target_interface = context.interface(resolved_type)
    if target_interface is None or target_interface.is_callback_interface:
        raise RuntimeError(f"{description} must have an interface type")
    if not cpp_type_for_idl_type_details(resolved_type, context).gc_ref_target_type:
        raise RuntimeError(f"{description} must convert from a GC pointer")

    incompatible_extended_attributes = (
        "CachedAttribute",
        "ImplementedInBindings",
        "LegacyUnforgeable",
        "Reflect",
        "ReturnsJSValue",
    )
    for extended_attribute in incompatible_extended_attributes:
        if extended_attribute in attribute.extended_attributes:
            raise RuntimeError(f"{description} cannot be combined with [{extended_attribute}]")


def reflected_attribute_name(attribute: Attribute) -> str:
    reflected = attribute.extended_attributes.get("Reflect")
    if reflected:
        return reflected.strip('"')
    return attribute.name.lower()


def is_dom_string_type(attribute: Attribute) -> bool:
    return attribute.type.name in ("CSSOMString", "DOMString")


def is_usv_string_type(attribute: Attribute) -> bool:
    return attribute.type.name == "USVString"


def attribute_callback_cpp_name(attribute: Attribute) -> str:
    return attribute.extended_attributes.get("AttributeCallbackName", idl_identifier_cpp_name(attribute))


def attribute_getter_callback_name(attribute: Attribute) -> str:
    return f"{attribute_callback_cpp_name(attribute)}_getter"


def attribute_setter_callback_name(attribute: Attribute) -> str:
    return f"{attribute_callback_cpp_name(attribute)}_setter"


def implementation_attribute_getter_call(
    interface: Interface, attribute: Attribute, realm_argument: str = "realm"
) -> str:
    method_name = idl_implementation_cpp_name(attribute)
    return f"""[&]<typename Implementation = {fully_qualified_name_for_interface(interface)}> {{
        auto& implementation = static_cast<Implementation&>(*idl_object);
        return Web::Bindings::invoke_first_available(implementation,
            [&](auto& implementation) -> decltype(implementation.{method_name}()) {{ return implementation.{method_name}(); }},
            [&](auto& implementation) -> decltype(implementation.{method_name}({realm_argument}.global_object())) {{ return implementation.{method_name}({realm_argument}.global_object()); }},
            [&](auto& implementation) -> decltype(implementation.{method_name}({realm_argument})) {{ return implementation.{method_name}({realm_argument}); }});
    }}()"""


def implementation_attribute_setter_call(
    context: GenerationContext, interface: Interface, attribute: Attribute, realm_argument: str = "realm"
) -> str:
    method_name = f"set_{idl_implementation_cpp_name(attribute)}"
    legacy_pointer_fallbacks = ""
    if cpp_type_for_idl_type_details(attribute.type, context).gc_ref_target_type:
        legacy_pointer_fallbacks = f""",
            [&](auto& implementation) -> decltype(implementation.{method_name}(idl_value.ptr())) {{ return implementation.{method_name}(idl_value.ptr()); }},
            [&](auto& implementation) -> decltype(implementation.{method_name}({realm_argument}, idl_value.ptr())) {{ return implementation.{method_name}({realm_argument}, idl_value.ptr()); }}"""
    return f"""[&]<typename Implementation = {fully_qualified_name_for_interface(interface)}> {{
        auto& implementation = static_cast<Implementation&>(*idl_object);
        return Web::Bindings::invoke_first_available(implementation,
            [&](auto& implementation) -> decltype(implementation.{method_name}(idl_value)) {{ return implementation.{method_name}(idl_value); }},
            [&](auto& implementation) -> decltype(implementation.{method_name}({realm_argument}, idl_value)) {{ return implementation.{method_name}({realm_argument}, idl_value); }}{legacy_pointer_fallbacks});
    }}()"""


def bindings_attribute_getter_call(attribute: Attribute, realm_argument: str) -> str:
    method_name = idl_implementation_cpp_name(attribute)
    return f"""Web::Bindings::invoke_first_available(*idl_object,
            [&](auto& implementation) -> decltype(Web::Bindings::{method_name}(implementation)) {{ return Web::Bindings::{method_name}(implementation); }},
            [&](auto& implementation) -> decltype(Web::Bindings::{method_name}({realm_argument}, implementation)) {{ return Web::Bindings::{method_name}({realm_argument}, implementation); }})"""


def bindings_attribute_setter_call(context: GenerationContext, attribute: Attribute, realm_argument: str) -> str:
    method_name = f"set_{idl_implementation_cpp_name(attribute)}"
    legacy_pointer_fallbacks = ""
    if cpp_type_for_idl_type_details(attribute.type, context).gc_ref_target_type:
        legacy_pointer_fallbacks = f""",
            [&](auto& implementation) -> decltype(Web::Bindings::{method_name}(implementation, idl_value.ptr())) {{ return Web::Bindings::{method_name}(implementation, idl_value.ptr()); }},
            [&](auto& implementation) -> decltype(Web::Bindings::{method_name}({realm_argument}, implementation, idl_value.ptr())) {{ return Web::Bindings::{method_name}({realm_argument}, implementation, idl_value.ptr()); }}"""
    return f"""Web::Bindings::invoke_first_available(*idl_object,
            [&](auto& implementation) -> decltype(Web::Bindings::{method_name}(implementation, idl_value)) {{ return Web::Bindings::{method_name}(implementation, idl_value); }},
            [&](auto& implementation) -> decltype(Web::Bindings::{method_name}({realm_argument}, implementation, idl_value)) {{ return Web::Bindings::{method_name}({realm_argument}, implementation, idl_value); }}{legacy_pointer_fallbacks})"""


def bindings_static_attribute_getter_call(attribute: Attribute) -> str:
    arguments = "realm" if "NeedsCallerRealm" in attribute.extended_attributes else "vm"
    return f"Web::Bindings::{idl_implementation_cpp_name(attribute)}({arguments})"


def define_the_regular_attributes(
    out: TextIO,
    includes: GeneratedIncludes,
    interface: Interface,
) -> None:
    # 1. Let attributes be the list of regular attributes that are members of definition.
    attributes = interface.regular_attributes

    # 2. Remove from attributes all the attributes that are unforgeable.
    attributes = [attribute for attribute in attributes if "LegacyUnforgeable" not in attribute.extended_attributes]

    # 3. Define the attributes attributes of definition on target given realm.
    define_the_attributes(out, includes, attributes, interface)


def define_the_unforgeable_attributes(
    out: TextIO,
    includes: GeneratedIncludes,
    interface: Interface,
) -> None:
    attributes = [
        attribute for attribute in interface.regular_attributes if "LegacyUnforgeable" in attribute.extended_attributes
    ]
    define_the_attributes(out, includes, attributes, interface)


def define_the_attributes(
    out: TextIO,
    includes: GeneratedIncludes,
    attributes: list[Attribute],
    interface: Interface,
) -> None:
    if not attributes:
        return

    out.write("\n")

    # 1. For each attribute attr of attributes:
    for attribute in attributes:
        if "FIXME" in attribute.extended_attributes:
            definition = f'    object.define_unimplemented_property("{attribute.name}"_utf16_fly_string);\n'
            out.write(
                wrap_with_extended_attribute_exposure_checks(
                    includes,
                    attribute.extended_attributes,
                    definition,
                )
            )
            continue

        getter_name = attribute_getter_callback_name(attribute)
        setter_name = attribute_setter_callback_name(attribute)
        cpp_name = attribute_callback_cpp_name(attribute)
        native_getter_name = f"native_{getter_name}"
        native_setter_name = f"native_{setter_name}"
        definition = StringIO()
        # 1. If attr is not exposed in realm, then continue.
        # NB: This is done at the end of this function.

        # 6. Let id be attr’s identifier.
        definition.write(f'    auto {cpp_name}_id = "{attribute.name}"_utf16_fly_string;\n')

        # 2. Let getter be the result of creating an attribute getter given attr, definition, and realm.
        if "LegacyUnforgeable" in attribute.extended_attributes:
            includes.add("LibWeb/Bindings/Intrinsics.h")
            definition.write(
                f'    auto {native_getter_name} = host_defined_intrinsics(realm).ensure_web_unforgeable_function("{interface.namespaced_name}"_utf16_fly_string, {cpp_name}_id, {getter_name}, UnforgeableKey::Type::Getter);\n'
            )
        elif "DirectGetter" in attribute.extended_attributes:
            direct_getter_field = attribute.extended_attributes.get("DirectGetter") or idl_identifier_cpp_name(
                attribute
            )
            includes.add("LibGC/Weak.h")
            includes.add("LibJS/Runtime/NativeFunction.h")
            includes.add("LibWeb/Bindings/PlatformObject.h")
            includes.add("LibWeb/Bindings/Wrappable.h")
            implementation_type = fully_qualified_name_for_interface(interface)
            definition.write(
                f"""    auto {native_getter_name} = JS::DirectGetterFunction::create(realm, {getter_name}, 0, {cpp_name}_id, {{
        .wrapper_implementation_offset = PlatformObject::wrapped_implementation_offset(),
        .implementation_value_offset = {implementation_type}::{direct_getter_field}_offset(),
        .main_world_wrapper_offset = Wrappable::main_world_wrapper_offset(),
        .weak_impl_value_offset = GC::WeakImpl::value_offset(),
    }}, "get"sv);
"""
            )
        else:
            definition.write(
                f'    auto {native_getter_name} = JS::NativeFunction::create(realm, {getter_name}, 0, {cpp_name}_id, &realm, "get"sv);\n'
            )

        # 3. Let setter be the result of creating an attribute setter given attr, definition, and realm.
        if not attribute_has_setter(attribute):
            # NB: the algorithm to create an attribute setter returns undefined if attr is read only.
            definition.write(f"    GC::Ptr<JS::NativeFunction> {native_setter_name};\n")
        else:
            if "LegacyUnforgeable" in attribute.extended_attributes:
                includes.add("LibWeb/Bindings/Intrinsics.h")
                definition.write(
                    f'    auto {native_setter_name} = host_defined_intrinsics(realm).ensure_web_unforgeable_function("{interface.namespaced_name}"_utf16_fly_string, {cpp_name}_id, {setter_name}, UnforgeableKey::Type::Setter);\n'
                )
            else:
                definition.write(
                    f'    auto {native_setter_name} = JS::NativeFunction::create(realm, {setter_name}, 1, {cpp_name}_id, &realm, "set"sv);\n'
                )
        define_accessor = f"object.define_direct_accessor({cpp_name}_id, {native_getter_name}, {native_setter_name}, {cpp_name}_attributes);"
        if "CachedAttribute" in attribute.extended_attributes and "LegacyUnforgeable" in attribute.extended_attributes:
            # Cache the result only on the main-world wrapper, whose cached
            # attributes can be invalidated directly when native state changes.
            # Non-main-world wrappers do not currently participate in that
            # invalidation path.
            includes.add("LibWeb/Bindings/WrapperWorld.h")
            define_accessor = f"""if (host_defined_wrapper_world(realm).is_main_world())
        object.define_direct_cached_accessor({cpp_name}_id, {native_getter_name}, {native_setter_name}, {cpp_name}_attributes);
    else
        object.define_direct_accessor({cpp_name}_id, {native_getter_name}, {native_setter_name}, {cpp_name}_attributes);"""
        definition.write(
            f"""
    // 4. Let configurable be false if attr is unforgeable and true otherwise.
    auto {cpp_name}_attributes = default_attributes;

    // 5. Let desc be the PropertyDescriptor{{[[Get]]: getter, [[Set]]: setter, [[Enumerable]]: true, [[Configurable]]: configurable}}.

    // 7. Perform ! DefinePropertyOrThrow(target, id, desc).
    {define_accessor}

    // 8. FIXME: If attr’s type is an observable array type with type argument T, then set target’s backing observable array exotic object for attr to the result of creating an observable array exotic object in realm, given T, attr’s set an indexed value algorithm, and attr’s delete an indexed value algorithm.
"""
        )
        out.write(
            wrap_with_extended_attribute_exposure_checks(
                includes,
                attribute.extended_attributes,
                definition.getvalue(),
                f"{interface.name}.{attribute.name}",
            )
        )


def define_the_static_attributes(out: TextIO, includes: GeneratedIncludes, interface: Interface) -> None:
    for attribute in interface.static_attributes:
        if "FIXME" in attribute.extended_attributes:
            definition = f'    object.define_unimplemented_property("{attribute.name}"_utf16_fly_string);\n'
            out.write(wrap_with_extended_attribute_exposure_checks(includes, attribute.extended_attributes, definition))
            continue
        definition = f'    object.define_native_accessor(realm, "{attribute.name}"_utf16_fly_string, {attribute_getter_callback_name(attribute)}, nullptr, default_attributes);\n'
        out.write(
            wrap_with_extended_attribute_exposure_checks(
                includes, attribute.extended_attributes, definition, f"{interface.name}.{attribute.name}"
            )
        )


def write_attribute_getters(
    out: TextIO, context: GenerationContext, includes: GeneratedIncludes, interface: Interface
) -> None:
    for attribute in interface.regular_attributes:
        if "FIXME" in attribute.extended_attributes:
            continue
        write_attribute_getter(out, context, includes, interface, attribute)


def write_attribute_getter(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
    attribute: Attribute,
    receiver_class: str | None = None,
) -> None:
    validate_direct_getter(context, interface, attribute)

    if receiver_class is None:
        receiver_class = interface.prototype_class

    attribute_type_is_promise = attribute.type.name == "Promise"
    if attribute_type_is_promise:
        includes.add("LibWeb/WebIDL/Promise.h")

    getter_prelude = ""
    getter_cache_check = ""
    getter_realm_argument = member_realm_expr(attribute, interface=interface)
    getter_steps = f"auto R = TRY(WebIDL::throw_dom_exception_if_needed(vm, {getter_realm_argument}, [&] {{ return {implementation_attribute_getter_call(interface, attribute, getter_realm_argument)}; }}));"
    is_reflected = "Reflect" in attribute.extended_attributes
    is_non_nullable_reflected = is_reflected and not attribute.type.nullable
    is_non_nullable_reflected_string = is_non_nullable_reflected and is_dom_string_type(attribute)
    is_reflected_usv_string = is_non_nullable_reflected and is_usv_string_type(attribute)

    if "ImplementedInBindings" in attribute.extended_attributes:
        includes.add(bindings_glue_header_for_interface(interface))
        getter_steps = f"auto R = TRY(WebIDL::throw_dom_exception_if_needed(vm, {getter_realm_argument}, [&] {{ return {bindings_attribute_getter_call(attribute, getter_realm_argument)}; }}));"
    elif is_reflected and attribute.type.name == "boolean":
        getter_steps = f"""// If a reflected IDL attribute has the type boolean:
    // 1. Let contentAttributeValue be the result of running this's get the content attribute.
    // 2. If contentAttributeValue is null, then return false.
    auto R = idl_object->has_attribute("{reflected_attribute_name(attribute)}"_utf16_fly_string);"""
    elif is_non_nullable_reflected and attribute.type.name == "long":
        includes.add("LibWeb/HTML/Numbers.h")
        getter_steps = f"""// If a reflected IDL attribute has the type long:
    // 1. Let contentAttributeValue be the result of running this's get the content attribute.
    // 2. If contentAttributeValue is not null:
    //    1. Let parsedValue be the result of integer parsing contentAttributeValue.
    //    2. If parsedValue is not an error and is within the long range, then return parsedValue.
    i32 R = 0;
    auto content_attribute_value = idl_object->get_attribute("{reflected_attribute_name(attribute)}"_utf16_fly_string);
    if (content_attribute_value.has_value()) {{
        auto maybe_parsed_value = Web::HTML::parse_integer(*content_attribute_value);
        if (maybe_parsed_value.has_value())
            R = *maybe_parsed_value;
    }}"""
    elif is_non_nullable_reflected and attribute.type.name == "unsigned long":
        includes.add("LibWeb/HTML/Numbers.h")
        getter_steps = f"""// If a reflected IDL attribute has the type unsigned long:
    // 1. Let contentAttributeValue be the result of running this's get the content attribute.
    // 2. Let minimum be 0.
    // FIXME: 3. If the reflected IDL attribute is limited to only positive numbers or limited to only positive numbers with fallback, then set minimum to 1.
    // FIXME: 4. If the reflected IDL attribute is clamped to the range, then set minimum to clampedMin.
    // 5. Let maximum be 2147483647 if the reflected IDL attribute is not clamped to the range; otherwise clampedMax.
    // 6. If contentAttributeValue is not null:
    //    1. Let parsedValue be the result of non-negative integer parsing contentAttributeValue.
    //    2. If parsedValue is not an error and is in the range minimum to maximum, inclusive, then return parsedValue.
    u32 R = 0;
    auto content_attribute_value = idl_object->get_attribute("{reflected_attribute_name(attribute)}"_utf16_fly_string);
    u32 minimum = 0;
    u32 maximum = 2147483647;
    if (content_attribute_value.has_value()) {{
        auto parsed_value = Web::HTML::parse_non_negative_integer(*content_attribute_value);
        if (parsed_value.has_value()) {{
            if (*parsed_value >= minimum && *parsed_value <= maximum)
                R = *parsed_value;
        }}
    }}"""
    elif is_reflected_usv_string:
        includes.add("AK/Utf16String.h")
        includes.add("LibWeb/Infra/Strings.h")
        getter_steps = f"""// If a reflected IDL attribute has the type USVString:
    // 1. Let element be the result of running this's get the element.
    // 2. Let contentAttributeValue be the result of running this's get the content attribute.
    auto content_attribute_value = idl_object->attribute("{reflected_attribute_name(attribute)}"_utf16_fly_string);

    // 3. Let attributeDefinition be the attribute definition of element's content attribute whose namespace is null and local name is the reflected content attribute name.

    // 5. Return contentAttributeValue, converted to a scalar value string.
    Utf16String R;
    if (content_attribute_value.has_value())
        R = MUST(Infra::convert_to_scalar_value_string(*content_attribute_value));"""
        if "URL" in attribute.extended_attributes:
            includes.add("LibWeb/DOM/Document.h")
            getter_steps = f"""// If a reflected IDL attribute has the type USVString:
    // 1. Let element be the result of running this's get the element.
    // 2. Let contentAttributeValue be the result of running this's get the content attribute.
    auto content_attribute_value = idl_object->attribute("{reflected_attribute_name(attribute)}"_utf16_fly_string);

    // 3. Let attributeDefinition be the attribute definition of element's content attribute whose namespace is null and local name is the reflected content attribute name.

    // 4. If attributeDefinition indicates it contains a URL:
    Utf16String R;
    if (content_attribute_value.has_value()) {{
        // 2. Let urlString be the result of encoding-parsing-and-serializing a URL given contentAttributeValue, relative to element's node document.
        auto url_string = idl_object->document().encoding_parse_and_serialize_url(*content_attribute_value);

        // 3. If urlString is not failure, then return urlString.
        if (url_string.has_value())
            R = url_string.release_value();
        else
            R = MUST(Infra::convert_to_scalar_value_string(*content_attribute_value));
    }}"""
    elif is_reflected and is_dom_string_type(attribute) and "Enumerated" in attribute.extended_attributes:
        includes.add("AK/Array.h")
        enumeration = context.enumeration(IDLType(attribute.extended_attributes["Enumerated"]))
        if enumeration is None:
            raise RuntimeError(
                f"Unknown reflected enumerated attribute type '{attribute.extended_attributes['Enumerated']}'"
            )
        valid_values = ", ".join(f'"{value}"_utf16' for value in enumeration.values)
        missing_value_default = enumeration.extended_attributes.get("MissingValueDefault", "")
        invalid_value_default = enumeration.extended_attributes.get("InvalidValueDefault", missing_value_default)
        if attribute.type.nullable:
            getter_steps = f"""// If a reflected IDL attribute is an enumerated attribute:
    // 1. Let contentAttributeValue be the result of running this's get the content attribute.
    auto R = idl_object->attribute("{reflected_attribute_name(attribute)}"_utf16_fly_string);

    // 3. If contentAttributeValue is an ASCII case-insensitive match for one of the keywords, then return that keyword's canonical keyword.
    Array valid_values {{ {valid_values} }};
    if (R.has_value()) {{
        auto has_keyword = false;
        for (auto const& value : valid_values) {{
            if (value.equals_ignoring_ascii_case(*R)) {{
                has_keyword = true;
                R = value;
                break;
            }}
        }}

        // 4. If contentAttributeValue is not a keyword, return the invalid value default.
        if (!has_keyword)
            R = "{invalid_value_default}"_utf16;
    }}"""
        else:
            getter_steps = f"""// If a reflected IDL attribute is an enumerated attribute:
    // 1. Let contentAttributeValue be the result of running this's get the content attribute.
    auto content_attribute_value = idl_object->attribute("{reflected_attribute_name(attribute)}"_utf16_fly_string);

    // 2. If contentAttributeValue is null, then set contentAttributeValue to the missing value default.
    auto R = content_attribute_value.value_or("{missing_value_default}"_utf16);
    auto did_set_to_missing_value = false;
    if (!content_attribute_value.has_value())
        did_set_to_missing_value = true;

    // 3. If contentAttributeValue is an ASCII case-insensitive match for one of the keywords, then return that keyword's canonical keyword.
    Array valid_values {{ {valid_values} }};
    auto has_keyword = false;
    for (auto const& value : valid_values) {{
        if (value.equals_ignoring_ascii_case(R)) {{
            has_keyword = true;
            R = value;
            break;
        }}
    }}

    // 4. If contentAttributeValue is not a keyword and was not set to the missing value default, return the invalid value default.
    if (!has_keyword && !did_set_to_missing_value)
        R = "{invalid_value_default}"_utf16;"""
    elif is_non_nullable_reflected_string:
        getter_steps = f"""// If a reflected IDL attribute has the type DOMString:
    // 1. Let element be the result of running this's get the element.
    // 2. Let contentAttributeValue be the result of running this's get the content attribute.
    // 5. If contentAttributeValue is null, then return the empty string.
    // 6. Return contentAttributeValue.
    auto R = idl_object->get_attribute_value("{reflected_attribute_name(attribute)}"_utf16_fly_string);"""
    elif attribute_is_nullable_reflected_element(attribute):
        includes.add("AK/Utf16FlyString.h")
        getter_steps = f"""static auto const& content_attribute = *new Utf16FlyString("{reflected_attribute_name(attribute)}"_utf16_fly_string);

    auto R = idl_object->get_the_attribute_associated_element(content_attribute, TRY(WebIDL::throw_dom_exception_if_needed(vm, *vm.current_realm(), [&] {{ return idl_object->{idl_implementation_cpp_name(attribute)}(); }})));"""
    elif attribute_is_nullable_reflected_frozen_array_of_element(attribute):
        includes.add("AK/Utf16FlyString.h")
        getter_steps = f"""static auto const& content_attribute = *new Utf16FlyString("{reflected_attribute_name(attribute)}"_utf16_fly_string);

    auto R = idl_object->get_the_attribute_associated_elements(content_attribute, TRY(WebIDL::throw_dom_exception_if_needed(vm, *vm.current_realm(), [&] {{ return idl_object->{idl_implementation_cpp_name(attribute)}(); }})));"""

    if "CachedAttribute" in attribute.extended_attributes and "LegacyUnforgeable" not in attribute.extended_attributes:
        includes.add("LibWeb/Bindings/WrapperWorld.h")
        includes.add(bindings_glue_header_for_interface(interface))
        getter_prelude = f"""    auto& wrapper_world = host_defined_wrapper_world({getter_realm_argument});
    if (auto cached_value = Bindings::cached_{idl_implementation_cpp_name(attribute)}(*idl_object, wrapper_world))
        return JS::Value(cached_value.ptr());

"""
    if attribute_is_nullable_reflected_frozen_array_of_element(attribute):
        includes.add("LibWeb/DOM/Element.h")
        includes.add("LibWeb/Bindings/WrapperWorld.h")
        getter_prelude = f"""    auto& wrapper_world = host_defined_wrapper_world({getter_realm_argument});

"""
        getter_cache_check = """    if (auto cached_value = cached_reflected_element_array(*idl_object, wrapper_world, content_attribute); cached_reflected_element_array_contains_same_elements(cached_value, R))
        return JS::Value(cached_value.ptr());

"""
    cached_return_value = None
    if attribute_uses_cached_js_value(attribute):
        cached_value = "&js_value.as_object()"
        if attribute_is_nullable_reflected_frozen_array_of_element(attribute):
            includes.add("LibJS/Runtime/Array.h")
            cached_value = "&as<JS::Array>(js_value.as_object())"
        cache_setter = f"idl_object->set_cached_{idl_implementation_cpp_name(attribute)}({cached_value});"
        if "CachedAttribute" in attribute.extended_attributes:
            cache_setter = f"Bindings::set_cached_{idl_implementation_cpp_name(attribute)}(*idl_object, wrapper_world, {cached_value});"
        elif attribute_is_nullable_reflected_frozen_array_of_element(attribute):
            cache_setter = (
                f"set_cached_reflected_element_array(*idl_object, wrapper_world, content_attribute, {cached_value});"
            )
        cached_return_value = f"""[&]() -> JS::Value {{
        JS::Value js_value = {to_javascript_value(attribute.type, "R", includes, context, getter_realm_argument)};
        if (js_value.is_object())
            {cache_setter}
        return js_value;
    }}()"""
    return_value = (
        "R"
        if "ReturnsJSValue" in attribute.extended_attributes
        else to_javascript_value(attribute.type, "R", includes, context, getter_realm_argument)
    )

    if attribute_type_is_promise:
        if getter_prelude or getter_cache_check:
            raise RuntimeError(f"Unsupported cached promise attribute '{attribute.name}' on '{interface.name}'")
        if getter_realm_argument == "realm":
            promise_realm_setup = "[[maybe_unused]] auto& promise_realm = realm;"
        else:
            promise_realm_setup = """auto promise_realm_this_value = vm.this_value();
    if (promise_realm_this_value.is_nullish())
        promise_realm_this_value = &realm.global_object();
    [[maybe_unused]] auto& promise_realm = this_value_realm(realm, promise_realm_this_value);"""
        out.write(
            f"""JS_DEFINE_NATIVE_FUNCTION({receiver_class}::{attribute_getter_callback_name(attribute)})
{{
    [[maybe_unused]] auto& realm = *vm.current_realm();
    {promise_realm_setup}

    auto steps = [&]() -> JS::ThrowCompletionOr<GC::Ptr<WebIDL::Promise>> {{
        [[maybe_unused]] {fully_qualified_name_for_interface(interface)}* idl_object = nullptr;

        auto js_value = vm.this_value();
        if (js_value.is_nullish())
            js_value = &realm.global_object();

        idl_object = TRY(impl_from(vm, js_value));
        [[maybe_unused]] auto& this_object_realm = this_value_realm(realm, js_value);

        {getter_steps}
        return R;
    }};

    auto maybe_R = steps();

    if (maybe_R.is_throw_completion())
        return WebIDL::create_rejected_promise(promise_realm, maybe_R.error_value())->promise();

    auto R = maybe_R.release_value();

    return {to_javascript_value(attribute.type, "R", includes, context, "promise_realm")};
}}

"""
        )
        return
    out.write(
        f"""JS_DEFINE_NATIVE_FUNCTION({receiver_class}::{attribute_getter_callback_name(attribute)})
{{
    [[maybe_unused]] auto& realm = *vm.current_realm();

    [[maybe_unused]] {fully_qualified_name_for_interface(interface)}* idl_object = nullptr;

    auto js_value = vm.this_value();
    if (js_value.is_nullish())
        js_value = &realm.global_object();

    idl_object = TRY(impl_from(vm, js_value));
    [[maybe_unused]] auto& this_object_realm = this_value_realm(realm, js_value);

{getter_prelude}
    {getter_steps}
{getter_cache_check}
    return {cached_return_value or return_value};
}}

"""
    )


def write_attribute_setters(
    out: TextIO, context: GenerationContext, includes: GeneratedIncludes, interface: Interface
) -> None:
    for attribute in interface.regular_attributes:
        if "FIXME" in attribute.extended_attributes:
            continue
        if not attribute_has_setter(attribute):
            continue
        write_attribute_setter(out, context, includes, interface, attribute)


def write_attribute_setter(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
    attribute: Attribute,
    receiver_class: str | None = None,
) -> None:
    if receiver_class is None:
        receiver_class = interface.prototype_class

    includes.add("LibJS/Runtime/Value.h")
    out.write(
        f"""JS_DEFINE_NATIVE_FUNCTION({receiver_class}::{attribute_setter_callback_name(attribute)})
{{
    [[maybe_unused]] auto& realm = *vm.current_realm();

    auto V = JS::js_undefined();
    if (vm.argument_count() > 0)
        V = vm.argument(0);

    [[maybe_unused]] {fully_qualified_name_for_interface(interface)}* idl_object = nullptr;

    auto js_value = vm.this_value();
    if (js_value.is_nullish())
        js_value = &realm.global_object();
    [[maybe_unused]] auto& this_object_realm = this_value_realm(realm, js_value);

    auto maybe_idl_object = impl_from(vm, js_value);
"""
    )
    if "LegacyLenientThis" not in attribute.extended_attributes:
        out.write(
            """    idl_object = TRY(maybe_idl_object);

"""
        )
    if "Replaceable" in attribute.extended_attributes:
        out.write(
            f"""    TRY(js_value.as_object().create_data_property_or_throw("{attribute.name}"_utf16_fly_string, V));

    return JS::js_undefined();
}}

"""
        )
        return

    if "LegacyLenientThis" in attribute.extended_attributes:
        out.write(
            """    if (maybe_idl_object.is_error())
        return JS::js_undefined();

    idl_object = maybe_idl_object.release_value();

"""
        )

    if "LegacyLenientSetter" in attribute.extended_attributes:
        out.write(
            """    return JS::js_undefined();
}

"""
        )
        return

    if put_forwards_identifier := attribute.extended_attributes.get("PutForwards"):
        includes.add("LibJS/Runtime/PropertyKey.h")
        out.write(
            f"""    auto receiver_value = TRY(js_value.as_object().get("{attribute.name}"_utf16_fly_string));

    if (!receiver_value.is_object())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObject, receiver_value);
    auto& receiver = receiver_value.as_object();

    auto forward_id = "{put_forwards_identifier}"_utf16_fly_string;

    TRY(receiver.set(JS::PropertyKey {{ forward_id, JS::PropertyKey::StringMayBeNumber::No }}, V, JS::Object::ShouldThrowExceptions::No));

    return JS::js_undefined();
}}

"""
        )
        return

    setter_realm_argument = member_realm_expr(attribute, interface=interface)
    setter_steps = f"TRY(WebIDL::throw_dom_exception_if_needed(vm, {setter_realm_argument}, [&] {{ return {implementation_attribute_setter_call(context, interface, attribute, setter_realm_argument)}; }}));\n    return {{}};"
    is_reflected = "Reflect" in attribute.extended_attributes
    is_non_nullable_reflected = is_reflected and not attribute.type.nullable
    is_nullable_reflected_string = is_reflected and attribute.type.nullable and is_dom_string_type(attribute)
    is_non_nullable_reflected_string = is_non_nullable_reflected and is_dom_string_type(attribute)
    is_reflected_usv_string = is_non_nullable_reflected and is_usv_string_type(attribute)

    def as_utf16_string_expression(value: str) -> str:
        return value

    if is_reflected and attribute.type.name == "boolean":
        setter_steps = f"""if (!idl_value)
        idl_object->remove_attribute("{reflected_attribute_name(attribute)}"_utf16_fly_string);
    else
        idl_object->set_attribute_value("{reflected_attribute_name(attribute)}"_utf16_fly_string, Utf16String {{}});
    return {{}};"""
    elif "ImplementedInBindings" in attribute.extended_attributes:
        includes.add(bindings_glue_header_for_interface(interface))
        setter_steps = f"TRY(WebIDL::throw_dom_exception_if_needed(vm, {setter_realm_argument}, [&] {{ return {bindings_attribute_setter_call(context, attribute, setter_realm_argument)}; }}));\n    return {{}};"
    elif is_non_nullable_reflected and attribute.type.name == "unsigned long":
        setter_steps = f"""u32 minimum = 0;
    u32 new_value = minimum;
    if (idl_value >= minimum && idl_value <= 2147483647)
        new_value = idl_value;
    idl_object->set_attribute_value("{reflected_attribute_name(attribute)}"_utf16_fly_string, Utf16String::number(new_value));
    return {{}};"""
    elif is_non_nullable_reflected and attribute.type.name == "long":
        setter_steps = f'idl_object->set_attribute_value("{reflected_attribute_name(attribute)}"_utf16_fly_string, Utf16String::number(idl_value));\n    return {{}};'
    elif is_reflected_usv_string or is_non_nullable_reflected_string:
        setter_steps = f'idl_object->set_attribute_value("{reflected_attribute_name(attribute)}"_utf16_fly_string, {as_utf16_string_expression("idl_value")});\n    return {{}};'
    elif is_nullable_reflected_string:
        setter_steps = f"""if (!idl_value.has_value())
        idl_object->remove_attribute("{reflected_attribute_name(attribute)}"_utf16_fly_string);
    else
        idl_object->set_attribute_value("{reflected_attribute_name(attribute)}"_utf16_fly_string, {as_utf16_string_expression("*idl_value")});
    return {{}};"""
    elif attribute_is_nullable_reflected_element(attribute):
        includes.add("AK/Utf16FlyString.h")
        setter_steps = f"""static auto const& content_attribute = *new Utf16FlyString("{reflected_attribute_name(attribute)}"_utf16_fly_string);

    if (!idl_value) {{
        idl_object->set_{idl_implementation_cpp_name(attribute)}({{}});
        idl_object->remove_attribute(content_attribute);
        return {{}};
    }}

    idl_object->set_attribute_value(content_attribute, Utf16String {{}});

    idl_object->set_{idl_implementation_cpp_name(attribute)}(*idl_value);
    return {{}};"""
    elif attribute_is_nullable_reflected_frozen_array_of_element(attribute):
        includes.add("LibWeb/DOM/Element.h")
        includes.add("AK/Utf16FlyString.h")
        includes.add("LibWeb/Bindings/WrapperWorld.h")
        includes.add("LibGC/Weak.h")
        setter_steps = f"""auto& wrapper_world = host_defined_wrapper_world(this_object_realm);

    static auto const& content_attribute = *new Utf16FlyString("{reflected_attribute_name(attribute)}"_utf16_fly_string);
    set_cached_reflected_element_array(*idl_object, wrapper_world, content_attribute, nullptr);

    if (!idl_value.has_value()) {{
        idl_object->set_{idl_implementation_cpp_name(attribute)}({{}});
        idl_object->remove_attribute(content_attribute);
        return {{}};
    }}

    idl_object->set_attribute_value(content_attribute, Utf16String {{}});

    Vector<GC::Weak<DOM::Element>> elements;
    elements.ensure_capacity(idl_value->size());

    for (auto const& element : *idl_value)
        elements.unchecked_append(*element);

    idl_object->set_{idl_implementation_cpp_name(attribute)}(move(elements));
    return {{}};"""
    out.write(
        """    auto original_steps = [&]() -> JS::ThrowCompletionOr<JS::Value> {
"""
    )
    out.write(
        f"""        JS::VM::TypeErrorRealmScope conversion_type_error_realm {{ vm, {setter_realm_argument} }};

"""
    )
    if context.enumeration(attribute.type) is not None:
        out.write(
            f"""        auto maybe_idl_value = WebIDL::throw_dom_exception_if_needed(vm, {setter_realm_argument}, [&] {{ return {to_idl_value(attribute, "V", includes, context)}; }});

        if (maybe_idl_value.is_error())
            return JS::js_undefined();

        auto idl_value = maybe_idl_value.release_value();
        conversion_type_error_realm.restore();

"""
        )
    else:
        out.write(
            f"""        auto idl_value = TRY(WebIDL::throw_dom_exception_if_needed(vm, {setter_realm_argument}, [&] {{ return {to_idl_value(attribute, "V", includes, context)}; }}));
        conversion_type_error_realm.restore();

"""
        )
    out.write(
        f"""        auto setter_result = [&]() -> JS::ThrowCompletionOr<void> {{
            {setter_steps}
        }}();

        if (setter_result.is_error())
            return setter_result.release_error();

        return JS::js_undefined();
    }};
"""
    )
    if "CEReactions" in attribute.extended_attributes:
        setter_result = wrap_with_ce_reactions(includes, "original_steps()", "this_object_realm")
    else:
        setter_result = "original_steps()"
    out.write(f"""
    return TRY({setter_result});
}}

""")


def write_static_attribute_getters(
    out: TextIO, context: GenerationContext, includes: GeneratedIncludes, interface: Interface
) -> None:
    for attribute in interface.static_attributes:
        if "FIXME" in attribute.extended_attributes:
            continue
        write_static_attribute_getter(out, context, includes, interface, attribute)


def write_static_attribute_getter(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
    attribute: Attribute,
) -> None:
    if "ImplementedInBindings" in attribute.extended_attributes:
        includes.add(bindings_glue_header_for_interface(interface))
        getter_call = bindings_static_attribute_getter_call(attribute)
    elif "NeedsCallerRealm" in attribute.extended_attributes:
        getter_call = (
            f"{fully_qualified_name_for_interface(interface)}::{idl_implementation_cpp_name(attribute)}(realm)"
        )
    else:
        method_name = idl_implementation_cpp_name(attribute)
        getter_call = f"""[&]<typename Implementation = {fully_qualified_name_for_interface(interface)}> {{
        return Web::Bindings::invoke_first_available_static<Implementation>(
            [&]<typename StaticImplementation>() -> decltype(StaticImplementation::{method_name}()) {{ return StaticImplementation::{method_name}(); }},
            [&]<typename StaticImplementation>() -> decltype(StaticImplementation::{method_name}(vm)) {{ return StaticImplementation::{method_name}(vm); }});
    }}()"""

    out.write(f"""JS_DEFINE_NATIVE_FUNCTION({interface.constructor_class}::{attribute_getter_callback_name(attribute)})
{{
    [[maybe_unused]] auto& realm = *vm.current_realm();

    // Let R be the result of running the getter steps of attribute.
    auto R = TRY(WebIDL::throw_dom_exception_if_needed(vm, *vm.current_realm(), [&] {{ return {getter_call}; }}));

    // Return the result of converting R to a JavaScript value of the type attribute is declared as.
    return {to_javascript_value(attribute.type, "R", includes, context)};
}}

""")
