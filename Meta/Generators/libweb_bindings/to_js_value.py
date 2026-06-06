# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from typing import TextIO

from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.cpp_types import add_binding_include_for_type
from Generators.libweb_bindings.cpp_types import add_buffer_source_type_include
from Generators.libweb_bindings.cpp_types import cpp_name
from Generators.libweb_bindings.cpp_types import cpp_type_details
from Generators.libweb_bindings.cpp_types import cpp_type_for_idl_type
from Generators.libweb_bindings.cpp_types import cpp_type_for_idl_type_details
from Generators.libweb_bindings.cpp_types import implementation_header_for_interface
from Generators.libweb_bindings.cpp_types import interface_like_type_for_idl_type
from Generators.libweb_bindings.cpp_types import is_buffer_source_type
from Generators.libweb_bindings.cpp_types import is_optional_without_default
from Generators.libweb_bindings.cpp_types import is_string_type
from Generators.libweb_bindings.includes import GeneratedIncludes
from Utils.utils import make_name_acceptable_cpp
from Utils.utils import string_to_cpp_enum_name
from Utils.utils import title_case_to_snake_case
from Utils.webidl_parser import Dictionary
from Utils.webidl_parser import DictionaryMember
from Utils.webidl_parser import Enumeration
from Utils.webidl_parser import IDLParameterizedType
from Utils.webidl_parser import IDLType
from Utils.webidl_parser import IDLUnionType


def unsupported_to_javascript_value(idl_type: IDLType) -> str:
    raise RuntimeError(f"Unsupported IDL value conversion for '{idl_type.name}'")


def to_value_function_name(dictionary: Dictionary) -> str:
    return f"{make_name_acceptable_cpp(title_case_to_snake_case(dictionary.name))}_to_value"


def write_enumeration_to_javascript_value_declaration(
    out: TextIO,
    enumeration: Enumeration,
    includes: GeneratedIncludes,
) -> None:
    includes.add("AK/String.h")

    out.write(f"String idl_enum_to_string({enumeration.name});\n\n")


def write_dictionary_to_javascript_value_declaration(out: TextIO, dictionary: Dictionary) -> None:
    if "GenerateToValue" not in dictionary.extended_attributes:
        return

    out.write(f"JS::Value {to_value_function_name(dictionary)}(JS::Realm&, {dictionary.name} const&);\n\n")


def write_enumeration_to_javascript_value_conversion(out: TextIO, enumeration: Enumeration) -> None:
    out.write(
        f"""// https://webidl.spec.whatwg.org/#idl-enumeration
String idl_enum_to_string({enumeration.name} value)
{{
    // The result of converting an IDL enumeration type value to a JavaScript value is the String value that represents the same sequence of code units as the enumeration value.
    switch (value) {{
"""
    )

    for value in enumeration.values:
        out.write(f"    case {enumeration.name}::{string_to_cpp_enum_name(value)}:\n")
        out.write(f'        return "{value}"_string;\n')

    out.write(
        """    }
    VERIFY_NOT_REACHED();
}

"""
    )


def write_dictionary_to_javascript_value_conversion(
    out: TextIO,
    dictionary: Dictionary,
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> None:
    if "GenerateToValue" not in dictionary.extended_attributes:
        return

    includes.add("LibJS/Runtime/Object.h")
    out.write(
        f"""JS::Value {to_value_function_name(dictionary)}(JS::Realm& realm, {dictionary.name} const& dictionary)
{{
    auto& vm = realm.vm();
    return {to_javascript_value(IDLType(dictionary.name), "dictionary", includes, context)};
}}

"""
    )


def integer_to_javascript_value(cpp_type_name: str, value: str, includes: GeneratedIncludes) -> str:
    includes.add("LibWeb/WebIDL/Types.h")
    return f"JS::Value(static_cast<{cpp_type_name}>({value}))"


# 3.2.1. any, https://webidl.spec.whatwg.org/#js-any
def any_to_javascript_value(value: str) -> str:
    # An IDL any value is converted to a JavaScript value according to the rules for converting the specific type of the
    # NB: We're getting passed a JS::Value - which is already our JS representation, so we can just return it as-is.
    return value


# 3.2.2. undefined, https://webidl.spec.whatwg.org/#js-undefined
def undefined_to_javascript_value() -> str:
    # The unique IDL undefined value is converted to the JavaScript undefined value.
    return "JS::js_undefined()"


# 3.2.3. boolean, https://webidl.spec.whatwg.org/#js-boolean
def boolean_to_javascript_value(value: str) -> str:
    # The IDL boolean value true is converted to the JavaScript true value and the IDL boolean value false is converted
    # to the JavaScript false value.
    return f"JS::Value({value})"


# 3.2.4.1. byte, https://webidl.spec.whatwg.org/#js-byte
def byte_to_javascript_value(value: str, includes: GeneratedIncludes) -> str:
    # The result of converting an IDL byte value to a JavaScript value is a Number that represents the same numeric value
    # as the IDL byte value. The Number value will be an integer in the range [−128, 127].
    return integer_to_javascript_value("WebIDL::Byte", value, includes)


# 3.2.4.2. octet, https://webidl.spec.whatwg.org/#js-octet
def octet_to_javascript_value(value: str, includes: GeneratedIncludes) -> str:
    # The result of converting an IDL octet value to a JavaScript value is a Number that represents the same numeric value
    # as the IDL octet value. The Number value will be an integer in the range [0, 255].
    return integer_to_javascript_value("WebIDL::Octet", value, includes)


# 3.2.4.3. short, https://webidl.spec.whatwg.org/#js-short
def short_to_javascript_value(value: str, includes: GeneratedIncludes) -> str:
    # The result of converting an IDL short value to a JavaScript value is a Number that represents the same numeric value
    # as the IDL short value. The Number value will be an integer in the range [−32768, 32767].
    return integer_to_javascript_value("WebIDL::Short", value, includes)


# 3.2.4.4. unsigned short, https://webidl.spec.whatwg.org/#js-unsigned-short
def unsigned_short_to_javascript_value(value: str, includes: GeneratedIncludes) -> str:
    # The result of converting an IDL unsigned short value to a JavaScript value is a Number that represents the same numeric
    # value as the IDL unsigned short value. The Number value will be an integer in the range [0, 65535].
    return integer_to_javascript_value("WebIDL::UnsignedShort", value, includes)


# 3.2.4.5. long, https://webidl.spec.whatwg.org/#js-long
def long_to_javascript_value(value: str, includes: GeneratedIncludes) -> str:
    # The result of converting an IDL long value to a JavaScript value is a Number that represents the same numeric value as
    # the IDL long value. The Number value will be an integer in the range [−2147483648, 2147483647].
    return integer_to_javascript_value("WebIDL::Long", value, includes)


# 3.2.4.6. unsigned long, https://webidl.spec.whatwg.org/#js-unsigned-long
def unsigned_long_to_javascript_value(value: str, includes: GeneratedIncludes) -> str:
    # The result of converting an IDL unsigned long value to a JavaScript value is a Number that represents the same numeric
    # value as the IDL unsigned long value. The Number value will be an integer in the range [0, 4294967295].
    return integer_to_javascript_value("WebIDL::UnsignedLong", value, includes)


# 3.2.4.7. long long,
def long_long_to_javascript_value(value: str, includes: GeneratedIncludes) -> str:
    # The result of converting an IDL long long value to a JavaScript value is a Number value that represents the closest
    # numeric value to the long long, choosing the numeric value with an even significand if there are two equally close
    # values. If the long long is in the range [−2^53 + 1, 2^53 − 1], then the Number will be able to represent exactly
    # the same value as the long long.
    return integer_to_javascript_value("double", value, includes)


# 3.2.4.8. unsigned long long, https://webidl.spec.whatwg.org/#js-unsigned-long-long
def unsigned_long_long_to_javascript_value(value: str, includes: GeneratedIncludes) -> str:
    # The result of converting an IDL unsigned long long value to a JavaScript value is a Number value that represents
    # the closest numeric value to the unsigned long long, choosing the numeric value with an even significand if there
    # are two equally close values. If the unsigned long long is less than or equal to 253 − 1, then the Number will be
    # able to represent exactly the same value as the unsigned long long.
    return integer_to_javascript_value("double", value, includes)


# 3.2.5. float, https://webidl.spec.whatwg.org/#js-float
def float_to_javascript_value(value: str) -> str:
    # The result of converting an IDL float value to a JavaScript value is the Number value that represents the same
    # numeric value as the IDL float value.
    return f"JS::Value({value})"


# 3.2.6. unrestricted float, https://webidl.spec.whatwg.org/#js-unrestricted-float
def unrestricted_float_to_javascript_value(value: str) -> str:
    # 1. If the IDL unrestricted float value is a NaN, then the Number value is NaN.
    # 2. Otherwise, the Number value is the one that represents the same numeric value as the IDL unrestricted float value.
    return f"JS::Value({value})"


# 3.2.7. double, https://webidl.spec.whatwg.org/#js-double
def double_to_javascript_value(value: str) -> str:
    # The result of converting an IDL double value to a JavaScript value is the Number value that represents the same
    # numeric value as the IDL double value.
    return f"JS::Value({value})"


# 3.2.8. unrestricted double, https://webidl.spec.whatwg.org/#js-unrestricted-double
def unrestricted_double_to_javascript_value(value: str) -> str:
    # 1. If the IDL unrestricted double value is a NaN, then the Number value is NaN.
    # 2. Otherwise, the Number value is the one that represents the same numeric value as the IDL unrestricted double value.
    return f"JS::Value({value})"


# 3.2.10. DOMString, https://webidl.spec.whatwg.org/#js-DOMString
def domstring_to_javascript_value(value: str, includes: GeneratedIncludes) -> str:
    includes.add("LibJS/Runtime/PrimitiveString.h")
    return f"JS::PrimitiveString::create(vm, {value})"


# 3.2.11. ByteString, https://webidl.spec.whatwg.org/#js-ByteString
def bytestring_to_javascript_value(value: str, includes: GeneratedIncludes) -> str:
    includes.add("LibJS/Runtime/PrimitiveString.h")
    # The result of converting an IDL ByteString value to a JavaScript value is a String value whose length is the length
    # of the ByteString, and the value of each element of which is the value of the corresponding element of the ByteString.
    return f"JS::PrimitiveString::create(vm, {value})"


# 3.2.12. USVString, https://webidl.spec.whatwg.org/#js-USVString
def usvstring_to_javascript_value(value: str, includes: GeneratedIncludes) -> str:
    includes.add("LibJS/Runtime/PrimitiveString.h")
    # The result of converting an IDL USVString value S to a JavaScript value is S.
    return f"JS::PrimitiveString::create(vm, {value})"


# 3.2.13. object, https://webidl.spec.whatwg.org/#js-object
def object_to_javascript_value(value: str) -> str:
    # The result of converting an IDL object value to a JavaScript value is the Object value that represents a reference to
    # the same object that the IDL object represents.
    return f"JS::Value({value})"


# 3.2.15. Interface types, https://webidl.spec.whatwg.org/#js-interface
def interface_to_javascript_value(value: str, includes: GeneratedIncludes, interface_like_type) -> str:
    includes.add(interface_like_type.implementation_header)
    # FIXME: Do we need this const cast?
    # The result of converting an IDL interface type value to a JavaScript value is the Object value that represents a
    # reference to the same object that the IDL interface type value represents.
    return f"JS::Value({value})"


# 3.2.16. Callback interface types, https://webidl.spec.whatwg.org/#js-callback-interface
def callback_interface_to_javascript_value(value: str, includes: GeneratedIncludes, interface) -> str:
    # The result of converting an IDL callback interface type value to a JavaScript value is the Object value that represents
    # a reference to the same object that the IDL callback interface type value represents.
    includes.add(implementation_header_for_interface(interface))
    return f"{value}->callback().callback"


def dictionary_member_to_javascript_conversion(
    member: DictionaryMember,
    dictionary_value: str,
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> tuple[str, str]:
    member_value = f"{dictionary_value}.{cpp_name(member)}"
    member_type = member.type
    member_exists = ""

    if is_optional_without_default(member):
        cpp_type = cpp_type_details(member, context)
        if cpp_type.gc_ref_target_type and not member.type.nullable:
            member_exists = member_value
        else:
            member_exists = f"{member_value}.has_value()"
            member_value = f"{member_value}.value()"
            if member.type.nullable and not isinstance(member.type, IDLUnionType) and not cpp_type.is_optional_presence:
                member_type = member.type.without_nullable()

    return member_exists, to_javascript_value(member_type, member_value, includes, context)


# 3.2.17. Dictionary types, https://webidl.spec.whatwg.org/#js-dictionary
def dictionary_to_javascript_value(
    idl_type: IDLType,
    value: str,
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> str:
    dictionary = context.dictionary(idl_type)
    if dictionary is None:
        raise RuntimeError(f"Unknown dictionary '{idl_type.name}'")
    add_binding_include_for_type(idl_type, includes, context)
    includes.add("LibJS/Runtime/Object.h")
    generated_conversion = """[&]() -> JS::Value {
        // 1. Let O be OrdinaryObjectCreate(%Object.prototype%).
        auto dictionary_object = JS::Object::create(realm, realm.intrinsics().object_prototype());
"""

    # 2. Let dictionaries be a list consisting of D and all of D's inherited dictionaries, in order from least to most derived.
    # 3. For each dictionary dictionary in dictionaries, in order:
    for dictionary_in_stack in reversed(context.dictionary_inheritance_stack(dictionary)):
        # 1. For each dictionary member member declared on dictionary, in lexicographical order:
        for member in dictionary_in_stack.members:
            member_exists, converted_member_value = dictionary_member_to_javascript_conversion(
                member, value, includes, context
            )

            generated_conversion += """
        // 1. Let key be the identifier of member.
        // 2. If V[key] exists, then:
"""
            if member_exists:
                generated_conversion += f"        if ({member_exists}) {{\n"

            generated_conversion += f"""
        // 1. Let idlValue be V[key].
        // 2. Let value be the result of converting idlValue to a JavaScript value.
        // 3. Perform ! CreateDataPropertyOrThrow(O, key, value).
        MUST(dictionary_object->create_data_property("{member.name}"_utf16_fly_string, {converted_member_value}));
"""
            if member_exists:
                generated_conversion += "        }\n"

    generated_conversion += """
        // 4. Return O.
        return dictionary_object;
    }()"""
    return generated_conversion


# 3.2.18. Enumeration types, https://webidl.spec.whatwg.org/#js-enumeration
def enumeration_to_javascript_value(
    idl_type: IDLType,
    value: str,
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> str:
    add_binding_include_for_type(idl_type, includes, context)
    includes.add("LibJS/Runtime/PrimitiveString.h")
    # The result of converting an IDL enumeration type value to a JavaScript value is the String value that represents the
    # same sequence of code units as the enumeration value.
    return f"JS::PrimitiveString::create(vm, idl_enum_to_string({value}))"


# 3.2.19. Callback function types, https://webidl.spec.whatwg.org/#js-callback-function
def callback_function_to_javascript_value(value: str, includes: GeneratedIncludes) -> str:
    includes.add("LibWeb/WebIDL/CallbackType.h")
    # The result of converting an IDL callback function type value to a JavaScript value is a reference to the same object
    # that the IDL callback function type value represents.
    return f"{value}->callback"


# 3.2.20. Nullable types — T?, https://webidl.spec.whatwg.org/#js-nullable-type
def nullable_to_javascript_value(
    idl_type: IDLType,
    value: str,
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> str:
    inner_type = idl_type.clone_with_nullable(False)
    value_is_nullable_pointer = bool(cpp_type_for_idl_type_details(idl_type, context).gc_ref_target_type)
    inner_value = value if value_is_nullable_pointer else f"{value}.value()"
    has_value = value if value_is_nullable_pointer else f"{value}.has_value()"
    return f"""[&]() -> JS::Value {{
        // 1. If the IDL nullable type T? value is null, then the JavaScript value is null.
        if (!{has_value})
            return JS::js_null();

        // 2. Otherwise, the JavaScript value is the result of converting the IDL nullable type value to the inner IDL type T.
        return JS::Value({to_javascript_value(inner_type, inner_value, includes, context)});
    }}()"""


# 3.2.21. Sequences — sequence<T>, https://webidl.spec.whatwg.org/#js-sequence
def sequence_to_javascript_value(
    sequence_type: IDLParameterizedType,
    value: str,
    includes: GeneratedIncludes,
    context: GenerationContext,
    freeze: bool = False,
) -> str:
    includes.add("LibJS/Runtime/Array.h")
    element_type = sequence_type.parameters[0]
    length_name = "sequence_length"
    array_name = "sequence_array"
    index_name = "sequence_index"
    element_name = "sequence_element"
    js_element_name = "js_sequence_element"
    converted_element = to_javascript_value(element_type, element_name, includes, context)

    freeze_array = ""
    if freeze:
        freeze_array = f"""
        MUST({array_name}->set_integrity_level(JS::Object::IntegrityLevel::Frozen));
"""

    return f"""[&]() -> JS::Value {{
        // An IDL sequence<T> value S is converted to a JavaScript value as follows:
        // 1. Let n be the length of S.
        auto {length_name} = {value}.size();

        // 2. Let A be a new Array object created as if by the expression [].
        auto {array_name} = MUST(JS::Array::create(realm, {length_name}));

        // 3. Initialize i to be 0.
        // 4. While i < n:
        for (size_t {index_name} = 0; {index_name} < {length_name}; ++{index_name}) {{
            // 1. Let V be the value in S at index i.
            auto& {element_name} = {value}.at({index_name});

            // 2. Let E be the result of converting V to a JavaScript value.
            JS::Value {js_element_name} = {converted_element};

            // 3. Let P be the result of calling ! ToString(i).
            // 4. Perform ! CreateDataPropertyOrThrow(A, P, E).
            MUST({array_name}->create_data_property(JS::PropertyKey {{ {index_name} }}, {js_element_name}));

            // 5. Set i to i + 1.
        }}
{freeze_array}
        // 5. Return A.
        return {array_name};
    }}()"""


# 3.2.23. Records — record<K, V>, https://webidl.spec.whatwg.org/#js-record
def record_to_javascript_value(
    record_type: IDLParameterizedType,
    value: str,
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> str:
    if len(record_type.parameters) != 2:
        raise RuntimeError("Record type must have two parameters")
    key_type = record_type.parameters[0]
    if not is_string_type(key_type.name):
        raise RuntimeError(f"Unsupported record key type '{key_type.name}'")

    includes.add("AK/Utf16FlyString.h")
    includes.add("LibJS/Runtime/Object.h")
    value_type = record_type.parameters[1]
    object_name = "record_object"
    key_name = "record_key"
    value_name = "record_value"
    converted_value = to_javascript_value(value_type, value_name, includes, context)

    return f"""[&]() -> JS::Value {{
        // 1. Let result be OrdinaryObjectCreate(%Object.prototype%).
        auto {object_name} = JS::Object::create(realm, realm.intrinsics().object_prototype());

        // 2. For each key → value of D:
        for (auto const& [{key_name}, {value_name}] : {value}) {{
            // 1. Let jsKey be key converted to a JavaScript value.
            // 2. Let jsValue be value converted to a JavaScript value.
            // 3. Let created be ! CreateDataProperty(result, jsKey, jsValue).
            // 4. Assert: created is true.
            MUST({object_name}->create_data_property(Utf16FlyString::from_utf8({key_name}), {converted_value}));
        }}

        // 3. Return result.
        return {object_name};
    }}()"""


# 3.2.24. Promise types — Promise<T>, https://webidl.spec.whatwg.org/#js-promise
def promise_to_javascript_value(value: str, includes: GeneratedIncludes) -> str:
    includes.add("AK/TypeCasts.h")
    includes.add("LibJS/Runtime/Promise.h")
    includes.add("LibWeb/WebIDL/Promise.h")
    # The result of converting an IDL promise type value to a JavaScript value is the value of the [[Promise]] field of the
    # record that IDL promise type represents.
    return f"GC::Ref {{ as<JS::Promise>(*{value}->promise()) }}"


# 3.2.25. Union types, https://webidl.spec.whatwg.org/#js-union
def union_to_javascript_value(
    union_type: IDLUnionType,
    value: str,
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> str:
    includes.add("AK/Variant.h")
    # An IDL union type value is converted to a JavaScript value according to the rules for converting the specific type of the
    # IDL union type value as described in this section (§ 3.2 JavaScript type mapping).
    conversions = []
    for index, member_type in enumerate(union_type.flattened_member_types()):
        if member_type.name == "undefined":
            continue
        inner_type = member_type.clone_with_nullable(False)
        visited_value = f"visited_union_value{index}"
        visited_cpp_type = cpp_type_for_idl_type(inner_type, context)
        converted_value = to_javascript_value(inner_type, visited_value, includes, context)
        conversions.append(
            f"""        [&]({visited_cpp_type} const& {visited_value}) -> JS::Value
        {{
            return {converted_value};
        }}"""
        )

    if union_type.includes_nullable_type():
        conversions.append(
            """        [](Empty) -> JS::Value
        {
            return JS::js_null();
        }"""
        )
    elif union_type.includes_undefined():
        conversions.append(
            """        [](Empty) -> JS::Value
        {
            return JS::js_undefined();
        }"""
        )

    joined_conversions = ",\n".join(conversions)
    return f"""{value}.visit(
{joined_conversions}
    )"""


# 3.2.26. Buffer source types, https://webidl.spec.whatwg.org/#js-buffer-source-types
def buffer_source_to_javascript_value(idl_type: IDLType, value: str, includes: GeneratedIncludes) -> str:
    add_buffer_source_type_include(idl_type, includes)
    # The result of converting an IDL value of any buffer source type to a JavaScript value is the Object value that
    # represents a reference to the same object that the IDL value represents.
    return f"JS::Value({value})"


# 3.2.27. Frozen arrays — FrozenArray<T>, https://webidl.spec.whatwg.org/#js-frozen-array
def frozen_array_to_javascript_value(
    frozen_array_type: IDLParameterizedType,
    value: str,
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> str:
    # The result of converting an IDL FrozenArray<T> value to a JavaScript value is the Object value that represents a
    # reference to the same object that the IDL FrozenArray<T> represents.
    return sequence_to_javascript_value(frozen_array_type, value, includes, context, freeze=True)


def to_javascript_value(
    idl_type: IDLType,
    value: str,
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> str:
    includes.add("LibJS/Runtime/Value.h")
    type_name = idl_type.name

    if isinstance(idl_type, IDLUnionType):
        return union_to_javascript_value(idl_type, value, includes, context)

    if idl_type.nullable:
        return nullable_to_javascript_value(idl_type, value, includes, context)

    if type_name == "undefined":
        return undefined_to_javascript_value()
    if type_name == "any":
        return any_to_javascript_value(value)
    if type_name == "object":
        return object_to_javascript_value(value)
    if is_buffer_source_type(idl_type):
        return buffer_source_to_javascript_value(idl_type, value, includes)
    if type_name == "boolean":
        return boolean_to_javascript_value(value)
    if type_name == "byte":
        return byte_to_javascript_value(value, includes)
    if type_name == "octet":
        return octet_to_javascript_value(value, includes)
    if type_name == "short":
        return short_to_javascript_value(value, includes)
    if type_name == "unsigned short":
        return unsigned_short_to_javascript_value(value, includes)
    if type_name == "long":
        return long_to_javascript_value(value, includes)
    if type_name == "unsigned long":
        return unsigned_long_to_javascript_value(value, includes)
    if type_name == "long long":
        return long_long_to_javascript_value(value, includes)
    if type_name == "unsigned long long":
        return unsigned_long_long_to_javascript_value(value, includes)
    if type_name == "float":
        return float_to_javascript_value(value)
    if type_name == "unrestricted float":
        return unrestricted_float_to_javascript_value(value)
    if type_name == "double":
        return double_to_javascript_value(value)
    if type_name == "unrestricted double":
        return unrestricted_double_to_javascript_value(value)

    if type_name in ("DOMString", "Utf16DOMString"):
        return domstring_to_javascript_value(value, includes)
    if type_name in ("ByteString",):
        return bytestring_to_javascript_value(value, includes)
    if type_name in ("USVString", "Utf16USVString"):
        return usvstring_to_javascript_value(value, includes)

    if context.enumeration(idl_type) is not None:
        return enumeration_to_javascript_value(idl_type, value, includes, context)

    if context.dictionary(idl_type) is not None:
        return dictionary_to_javascript_value(idl_type, value, includes, context)

    if type_name == "Promise":
        return promise_to_javascript_value(value, includes)

    interface_like_type = interface_like_type_for_idl_type(idl_type, context)
    if interface_like_type is not None:
        return interface_to_javascript_value(value, includes, interface_like_type)

    interface = context.interface(idl_type)
    if interface is not None and interface.is_callback_interface:
        return callback_interface_to_javascript_value(value, includes, interface)

    if context.callback_function(idl_type) is not None:
        return callback_function_to_javascript_value(value, includes)

    if isinstance(idl_type, IDLParameterizedType) and type_name == "sequence":
        return sequence_to_javascript_value(idl_type, value, includes, context)

    if isinstance(idl_type, IDLParameterizedType) and type_name == "record":
        return record_to_javascript_value(idl_type, value, includes, context)

    if isinstance(idl_type, IDLParameterizedType) and type_name == "FrozenArray":
        return frozen_array_to_javascript_value(idl_type, value, includes, context)

    return unsupported_to_javascript_value(idl_type)
