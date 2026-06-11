# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from dataclasses import dataclass
from dataclasses import field
from typing import List
from typing import NoReturn
from typing import Optional
from typing import TextIO

from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.cpp_types import DictionaryMemberOrAttribute
from Generators.libweb_bindings.cpp_types import InterfaceLikeType
from Generators.libweb_bindings.cpp_types import add_binding_include_for_type
from Generators.libweb_bindings.cpp_types import add_buffer_source_type_include
from Generators.libweb_bindings.cpp_types import add_header_includes_for_type
from Generators.libweb_bindings.cpp_types import add_include_for_string_cpp_type
from Generators.libweb_bindings.cpp_types import converter_function_name
from Generators.libweb_bindings.cpp_types import cpp_empty_value
from Generators.libweb_bindings.cpp_types import cpp_name
from Generators.libweb_bindings.cpp_types import cpp_type
from Generators.libweb_bindings.cpp_types import cpp_type_for_idl_type
from Generators.libweb_bindings.cpp_types import cpp_type_for_idl_type_details
from Generators.libweb_bindings.cpp_types import cpp_type_name_for_string
from Generators.libweb_bindings.cpp_types import cpp_value_type
from Generators.libweb_bindings.cpp_types import fully_qualified_name_for_interface
from Generators.libweb_bindings.cpp_types import implementation_header_for_interface
from Generators.libweb_bindings.cpp_types import interface_like_type_for_idl_type
from Generators.libweb_bindings.cpp_types import is_buffer_source_type
from Generators.libweb_bindings.cpp_types import is_numeric_type
from Generators.libweb_bindings.cpp_types import is_string_type
from Generators.libweb_bindings.cpp_types import is_typed_array_type
from Generators.libweb_bindings.cpp_types import union_type_to_variant
from Generators.libweb_bindings.default_values import cpp_default_value_conversion
from Generators.libweb_bindings.includes import GeneratedIncludes
from Utils.utils import make_name_acceptable_cpp
from Utils.utils import string_to_cpp_enum_name
from Utils.utils import title_case_to_snake_case
from Utils.utils import underlying_type_for_enum
from Utils.webidl_parser import CallbackFunction
from Utils.webidl_parser import Dictionary
from Utils.webidl_parser import Enumeration
from Utils.webidl_parser import IDLParameterizedType
from Utils.webidl_parser import IDLType
from Utils.webidl_parser import IDLUnionType
from Utils.webidl_parser import Interface


@dataclass
class UnionTypes:
    boolean_type: Optional[IDLType] = None
    numeric_type: Optional[IDLType] = None
    string_type: Optional[IDLType] = None
    bigint_type: Optional[IDLType] = None
    array_buffer_type: Optional[IDLType] = None
    data_view_type: Optional[IDLType] = None
    sequence_type: Optional[IDLParameterizedType] = None
    frozen_array_type: Optional[IDLParameterizedType] = None
    record_type: Optional[IDLParameterizedType] = None
    dictionary_type: Optional[IDLType] = None

    enumeration_types: List[IDLType] = field(default_factory=list)
    interface_types: List[IDLType] = field(default_factory=list)
    callback_function_types: List[IDLType] = field(default_factory=list)
    callback_interface_types: List[IDLType] = field(default_factory=list)
    typed_array_types: List[IDLType] = field(default_factory=list)

    includes_object_type: bool = False


def collect_union_types(flattened_member_types: List[IDLType], context: GenerationContext) -> UnionTypes:
    types = UnionTypes()

    for member_type in flattened_member_types:
        if types.boolean_type is None and member_type.name == "boolean":
            types.boolean_type = member_type
        if types.numeric_type is None and is_numeric_type(member_type.name):
            types.numeric_type = member_type
        if types.string_type is None and is_string_type(member_type.name):
            types.string_type = member_type
        if types.bigint_type is None and member_type.name == "bigint":
            types.bigint_type = member_type

        if context.enumeration(member_type) is not None:
            types.enumeration_types.append(member_type)
        if interface_like_type_for_idl_type(member_type, context) is not None:
            types.interface_types.append(member_type)
        if context.callback_function(member_type) is not None:
            types.callback_function_types.append(member_type)

        interface = context.interface(member_type)
        if interface is not None and interface.is_callback_interface:
            types.callback_interface_types.append(member_type)

        if member_type.name == "object":
            types.includes_object_type = True
        if types.array_buffer_type is None and member_type.name == "ArrayBuffer":
            types.array_buffer_type = member_type
        if types.data_view_type is None and member_type.name == "DataView":
            types.data_view_type = member_type
        if is_typed_array_type(member_type):
            types.typed_array_types.append(member_type)
        if types.dictionary_type is None and context.dictionary(member_type) is not None:
            types.dictionary_type = member_type

        if not isinstance(member_type, IDLParameterizedType):
            continue

        if types.sequence_type is None and member_type.name == "sequence":
            types.sequence_type = member_type
        if types.frozen_array_type is None and member_type.name == "FrozenArray":
            types.frozen_array_type = member_type
        if types.record_type is None and member_type.name == "record":
            types.record_type = member_type

    return types


def unsupported_to_idl_value(idl_type: IDLType) -> str:
    raise RuntimeError(f"Unsupported IDL value conversion for '{idl_type}'")


def idl_value_conversion_is_throwing(idl_type: IDLType, context: GenerationContext) -> bool:
    return idl_type.name not in ("any", "boolean")


def write_enumeration_declaration(out: TextIO, enumeration: Enumeration, includes: GeneratedIncludes) -> None:
    includes.add("LibJS/Forward.h")
    includes.add("LibJS/Runtime/Value.h")

    out.write(f"enum class {enumeration.name} : {underlying_type_for_enum(len(enumeration.values))} {{\n")
    for value in enumeration.values:
        out.write(f"    {string_to_cpp_enum_name(value)},\n")
    out.write("};\n\n")
    out.write(
        f"JS::ThrowCompletionOr<{enumeration.name}> {converter_function_name(enumeration)}(JS::VM&, JS::Value);\n\n"
    )


def write_dictionary_declaration(
    out: TextIO,
    dictionary: Dictionary,
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> None:
    includes.add("LibJS/Forward.h")
    includes.add("LibJS/Runtime/Value.h")
    parent_dictionary = context.dictionary_parent(dictionary)
    if parent_dictionary is not None:
        includes.add_binding(parent_dictionary.path.stem)

    parent = f" : public {dictionary.parent_name}" if dictionary.parent_name else ""
    out.write(f"struct {dictionary.name}{parent} {{\n")
    for member in dictionary.members:
        add_header_includes_for_type(member, includes, context)

        default_value = ""
        if member.default_value is not None:
            default_value = f" {cpp_default_value_conversion(member, context)} "
        member_cpp_type = cpp_type(member, context)
        if member.required and member.default_value is None:
            out.write(f"    {member_cpp_type} {cpp_name(member)};\n")
        else:
            out.write(f"    {member_cpp_type} {cpp_name(member)} {{{default_value}}};\n")
    out.write("};\n\n")
    out.write(
        f"JS::ThrowCompletionOr<{dictionary.name}> {converter_function_name(dictionary)}(JS::VM&, JS::Value);\n\n"
    )


def dictionaries_in_dependency_order(dictionaries: List[Dictionary], context: GenerationContext) -> List[Dictionary]:
    local_dictionaries = {dictionary.name: dictionary for dictionary in dictionaries}
    emitted: set[str] = set()
    visiting: set[str] = set()
    ordered_dictionaries: List[Dictionary] = []

    def dependency_names_for(dictionary: Dictionary) -> set[str]:
        dependency_names = {dictionary.parent_name} if dictionary.parent_name else set()
        for member in dictionary.members:
            dependency_names.update(context.dictionary_type_names(member.type))
        dependency_names.discard(dictionary.name)
        return dependency_names

    def visit(dictionary: Dictionary) -> None:
        if dictionary.name in emitted:
            return
        if dictionary.name in visiting:
            raise RuntimeError(f"Dictionary '{dictionary.name}' depends on itself")

        visiting.add(dictionary.name)
        for dependency_name in dependency_names_for(dictionary):
            dependency = local_dictionaries.get(dependency_name)
            if dependency is not None:
                visit(dependency)
        visiting.remove(dictionary.name)

        ordered_dictionaries.append(dictionary)
        emitted.add(dictionary.name)

    for dictionary in local_dictionaries.values():
        visit(dictionary)

    return ordered_dictionaries


def write_enumeration_conversion(out: TextIO, enumeration: Enumeration, includes: GeneratedIncludes) -> None:
    includes.add("LibJS/Runtime/Error.h")
    includes.add("LibJS/Runtime/VM.h")
    includes.add("LibJS/Runtime/ValueInlines.h")

    out.write(
        f"""// https://webidl.spec.whatwg.org/#idl-enumeration
JS::ThrowCompletionOr<{enumeration.name}> {converter_function_name(enumeration)}(JS::VM& vm, JS::Value value)
{{
    // 1. Let S be the result of calling ? ToString(V).
    auto value_as_string = TRY(value.to_string(vm));

    // 2. If S is not one of E’s enumeration values, then throw a TypeError.
    // 3. Return the enumeration value of type E that is equal to S.
"""
    )

    for value in enumeration.values:
        out.write(f'    if (value_as_string == "{value}"sv)\n')
        out.write(f"        return {enumeration.name}::{string_to_cpp_enum_name(value)};\n")
    out.write(
        f"""    return vm.throw_completion<JS::TypeError>(JS::ErrorType::InvalidEnumerationValue, value_as_string, "{enumeration.name}");
}}

"""
    )


def write_dictionary_conversion(
    out: TextIO,
    dictionary: Dictionary,
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> None:
    includes.add("LibJS/Runtime/Error.h")
    includes.add("LibJS/Runtime/VM.h")
    includes.add("LibJS/Runtime/Value.h")
    includes.add("LibJS/Runtime/ValueInlines.h")
    includes.add("LibWeb/Bindings/ExceptionOrUtils.h")

    parent_dictionary = context.dictionary_parent(dictionary)

    out.write(f"""// https://webidl.spec.whatwg.org/#es-dictionary
JS::ThrowCompletionOr<{dictionary.name}> {converter_function_name(dictionary)}(JS::VM& vm, JS::Value js_dict)
{{
    // 1. If jsDict is not an Object and jsDict is neither undefined nor null, then throw a TypeError.
    if (!js_dict.is_object() && !js_dict.is_nullish())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "{dictionary.name}");

    // 2. Let idlDict be an empty ordered map, representing a dictionary of type D.
    // 3. Let dictionaries be a list consisting of D and all of D’s inherited dictionaries, in order from least to most derived.
    // 4. For each dictionary dictionary in dictionaries, in order:
    // NB: We defer construction until the return initializer because some members may not be default-constructible. Inherited dictionaries are represented by the generated C++ struct inheritance.

    // 5. Return idlDict.
    return {dictionary.name} {{
""")
    if parent_dictionary is not None:
        out.write(f"        TRY({converter_function_name(parent_dictionary)}(vm, js_dict)),\n")

    # 4.1. For each dictionary member member declared on dictionary, in lexicographical order:
    for member in dictionary.members:
        conversion = to_idl_value(member, "js_member_value", includes, context)
        member_cpp_type = cpp_type(member, context)
        member_designator = "" if parent_dictionary is not None else f".{cpp_name(member)} = "
        out.write(
            f"""        {member_designator}TRY([&]() -> JS::ThrowCompletionOr<{member_cpp_type}> {{
            // 1. Let key be the identifier of member.
            // 2. If jsDict is either undefined or null, then:
            //     1. Let jsMemberValue be undefined.
            // 3. Otherwise,
            //     1. Let jsMemberValue be ? Get(jsDict, key).
            auto js_member_value = JS::js_undefined();
            if (js_dict.is_object())
                js_member_value = TRY(js_dict.as_object().get("{member.name}"_utf16_fly_string));

            // 4. If jsMemberValue is not undefined, then:
            if (!js_member_value.is_undefined()) {{
                // 1. Let idlMemberValue be the result of converting jsMemberValue to an IDL value whose type is the type member is declared to be of.
                auto idl_member_value = TRY(throw_dom_exception_if_needed(vm, [&] {{ return {conversion}; }}));

                // 2. Set idlDict[key] to idlMemberValue.
                return idl_member_value;
            }}
"""
        )
        if member.default_value is not None:
            out.write(
                f"""            // 5. Otherwise, if jsMemberValue is undefined but member has a default value, then:
            // 1. Let idlMemberValue be the result of converting member's default value to an IDL value whose type is the type member is declared to be of.
            auto idl_member_value = {cpp_default_value_conversion(member, context)};

            // 2. Set idlDict[key] to idlMemberValue.
            return idl_member_value;
"""
            )
        elif member.required:
            out.write(
                f"""            // 6. Otherwise, if jsMemberValue is undefined and member is required, then throw a TypeError.
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::MissingRequiredProperty, "{member.name}");
"""
            )
        else:
            out.write(
                f"""            // 7. Otherwise, jsMemberValue is undefined and the member is optional.
            return {cpp_empty_value(member, context)};
"""
            )

        out.write("        }()),\n")
    out.write(
        """    };
}

"""
    )


# 3.2.1. any, https://webidl.spec.whatwg.org/#js-any
def any_to_idl_value(value_name: str, includes: GeneratedIncludes) -> str:
    includes.add("LibJS/Runtime/Value.h")

    # 1. If V is undefined, then return the unique undefined IDL value.
    # 2. If V is null, then return the null object? reference.
    # 3. If V is a Boolean, then return the boolean value that represents the same truth value.
    # 4. If V is a Number, then return the result of converting V to an unrestricted double.
    # 5. If V is a BigInt, then return the result of converting V to a bigint.
    # 6. If V is a String, then return the result of converting V to a DOMString.
    # 7. If V is a Symbol, then return the result of converting V to a symbol.
    # 8. If V is an Object, then return an IDL object value that references V.
    # NB: We're getting passed a JS::Value - which is already our C++ representation, so we can just return it as-is.
    return value_name


# 3.2.3. boolean, https://webidl.spec.whatwg.org/#js-boolean
def boolean_to_idl_value(value_name: str, includes: GeneratedIncludes) -> str:
    includes.add("LibJS/Runtime/Value.h")

    # 1. Let x be the result of computing ToBoolean(V).
    # 2. Return the IDL boolean value that is the one that represents the same truth value as the JavaScript Boolean value x.
    return f"{value_name}.to_boolean()"


# https://webidl.spec.whatwg.org/#abstract-opdef-converttoint
def convert_to_int(
    cpp_type: str,
    value_name: str,
    includes: GeneratedIncludes,
    extended_attributes: dict[str, str],
) -> str:
    includes.add("LibWeb/WebIDL/Types.h")
    includes.add("LibWeb/WebIDL/AbstractOperations.h")

    enforce_range = "Yes" if "EnforceRange" in extended_attributes else "No"
    clamp = "Yes" if "Clamp" in extended_attributes else "No"
    return (
        f"WebIDL::convert_to_int<{cpp_type}>(vm, {value_name}, "
        f"WebIDL::EnforceRange::{enforce_range}, WebIDL::Clamp::{clamp})"
    )


# 3.2.4.1. byte, https://webidl.spec.whatwg.org/#js-byte
def byte_to_idl_value(value_name: str, includes: GeneratedIncludes, extended_attributes: dict[str, str]) -> str:
    # 1. Let x be ? ConvertToInt(V, 8, "signed").
    # 2. Return the IDL byte value that represents the same numeric value as x.
    return convert_to_int("WebIDL::Byte", value_name, includes, extended_attributes)


# 3.2.4.2. octet, https://webidl.spec.whatwg.org/#js-octet
def octet_to_idl_value(value_name: str, includes: GeneratedIncludes, extended_attributes: dict[str, str]) -> str:
    # 1. Let x be ? ConvertToInt(V, 8, "unsigned").
    # 2. Return the IDL octet value that represents the same numeric value as x.
    return convert_to_int("WebIDL::Octet", value_name, includes, extended_attributes)


# 3.2.4.3. short, https://webidl.spec.whatwg.org/#js-short
def short_to_idl_value(value_name: str, includes: GeneratedIncludes, extended_attributes: dict[str, str]) -> str:
    # 1. Let x be ? ConvertToInt(V, 16, "signed").
    # 2. Return the IDL short value that represents the same numeric value as x.
    return convert_to_int("WebIDL::Short", value_name, includes, extended_attributes)


# 3.2.4.4. unsigned short, https://webidl.spec.whatwg.org/#js-unsigned-short
def unsigned_short_to_idl_value(
    value_name: str, includes: GeneratedIncludes, extended_attributes: dict[str, str]
) -> str:
    # 1. Let x be ? ConvertToInt(V, 16, "unsigned").
    # 2. Return the IDL unsigned short value that represents the same numeric value as x.
    return convert_to_int("WebIDL::UnsignedShort", value_name, includes, extended_attributes)


# 3.2.4.5. long, https://webidl.spec.whatwg.org/#js-long
def long_to_idl_value(value_name: str, includes: GeneratedIncludes, extended_attributes: dict[str, str]) -> str:
    # 1. Let x be ? ConvertToInt(V, 32, "signed").
    # 2. Return the IDL long value that represents the same numeric value as x.
    return convert_to_int("WebIDL::Long", value_name, includes, extended_attributes)


# 3.2.4.6. unsigned long, https://webidl.spec.whatwg.org/#js-unsigned-long
def unsigned_long_to_idl_value(
    value_name: str, includes: GeneratedIncludes, extended_attributes: dict[str, str]
) -> str:
    # 1. Let x be ? ConvertToInt(V, 32, "unsigned").
    # 2. Return the IDL unsigned long value that represents the same numeric value as x.
    return convert_to_int("WebIDL::UnsignedLong", value_name, includes, extended_attributes)


# 3.2.4.7. long long, https://webidl.spec.whatwg.org/#js-long-long
def long_long_to_idl_value(value_name: str, includes: GeneratedIncludes, extended_attributes: dict[str, str]) -> str:
    # 1. Let x be ? ConvertToInt(V, 64, "signed").
    # 2. Return the IDL long long value that represents the same numeric value as x.
    return convert_to_int("WebIDL::LongLong", value_name, includes, extended_attributes)


# 3.2.4.8. unsigned long long, https://webidl.spec.whatwg.org/#js-unsigned-long-long
def unsigned_long_long_to_idl_value(
    value_name: str,
    includes: GeneratedIncludes,
    extended_attributes: dict[str, str],
) -> str:
    # 1. Let x be ? ConvertToInt(V, 64, "unsigned").
    # 2. Return the IDL unsigned long long value that represents the same numeric value as x.
    return convert_to_int("WebIDL::UnsignedLongLong", value_name, includes, extended_attributes)


# 3.2.5. float, https://webidl.spec.whatwg.org/#js-float
def float_to_idl_value(value_name: str, includes: GeneratedIncludes, identifier: str) -> str:
    includes.add("LibJS/Runtime/Error.h")
    includes.add("LibJS/Runtime/ValueInlines.h")

    # 1. Let x be ? ToNumber(V).
    # 2. If x is NaN, +∞, or −∞, then throw a TypeError.
    # 3. Let S be the set of finite IEEE 754 single-precision floating point values except −0, but with two special values added: 2^128 and −2^128.
    # 4. Let y be the number in S that is closest to x, selecting the number with an even significand if there are two equally close values. (The two special values 2^128 and −2^128 are considered to have even significands for this purpose.)
    # 5. If y is 2^128 or −2^128, then throw a TypeError.
    # 6. If y is +0 and x is negative, return −0.
    # 7. Return y.
    # FIXME: Correctly implement steps 3-7.
    return f"""[&]() -> JS::ThrowCompletionOr<float> {{
        float x = TRY({value_name}.to_double(vm));
        if (isinf(x) || isnan(x))
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::InvalidRestrictedFloatingPointParameter, "{identifier}");
        return x;
    }}()"""


# 3.2.6. unrestricted float, https://webidl.spec.whatwg.org/#js-unrestricted-float
def unrestricted_float_to_idl_value(value_name: str, includes: GeneratedIncludes) -> str:
    includes.add("LibJS/Runtime/ValueInlines.h")

    # 1. Let x be ? ToNumber(V).
    # 2. If x is NaN, then return the IDL unrestricted float value that represents the IEEE 754 NaN value with the bit pattern 0x7fc00000 [IEEE-754].
    # 3. Let S be the set of finite IEEE 754 single-precision floating point values except −0, but with two special values added: 2^128 and −2^128.
    # 4. Let y be the number in S that is closest to x, selecting the number with an even significand if there are two equally close values. (The two special values 2^128 and −2^128 are considered to have even significands for this purpose.)
    # 5. If y is 2^128, return +∞.
    # 6. If y is −2^128, return −∞.
    # 7. If y is +0 and x is negative, return −0.
    # 8. Return y.
    # FIXME: Correctly implement steps 2-8.
    return f"""[&]() -> JS::ThrowCompletionOr<float> {{
        return static_cast<float>(TRY({value_name}.to_double(vm)));
    }}()"""


# 3.2.7. double, https://webidl.spec.whatwg.org/#js-double
def double_to_idl_value(value_name: str, includes: GeneratedIncludes, identifier: str) -> str:
    includes.add("LibJS/Runtime/Error.h")
    includes.add("LibJS/Runtime/ValueInlines.h")

    # 1. Let x be ? ToNumber(V).
    # 2. If x is NaN, +∞, or −∞, then throw a TypeError.
    # 3. Return the IDL double value that represents the same numeric value as x.
    return f"""[&]() -> JS::ThrowCompletionOr<double> {{
        auto x = TRY({value_name}.to_double(vm));
        if (isinf(x) || isnan(x))
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::InvalidRestrictedFloatingPointParameter, "{identifier}");
        return x;
    }}()"""


# 3.2.8. unrestricted double, https://webidl.spec.whatwg.org/#js-unrestricted-double
def unrestricted_double_to_idl_value(value_name: str, includes: GeneratedIncludes) -> str:
    includes.add("LibJS/Runtime/ValueInlines.h")
    # 1. Let x be ? ToNumber(V).
    # 2. If x is NaN, then return the IDL unrestricted double value that represents the IEEE 754 NaN value with the bit pattern 0x7ff8000000000000 [IEEE-754].
    # 3. Return the IDL unrestricted double value that represents the same numeric value as x.
    # FIXME!
    return f"{value_name}.to_double(vm)"


# 3.2.9. bigint, https://webidl.spec.whatwg.org/#js-bigint
def bigint_to_idl_value(value_name: str, includes: GeneratedIncludes) -> str:
    includes.add("LibJS/Runtime/BigInt.h")
    includes.add("LibJS/Runtime/Value.h")
    includes.add("LibJS/Runtime/ValueInlines.h")

    # 1. Let x be ? ToBigInt(V).
    # 2. Return the IDL bigint value that represents the same numeric value as x.
    return f"{value_name}.to_bigint(vm)"


# 3.2.10. DOMString, https://webidl.spec.whatwg.org/#js-DOMString
def dom_string_to_idl_value(
    value_name: str,
    includes: GeneratedIncludes,
    extended_attributes: Optional[dict[str, str]] = None,
    type_name: str = "DOMString",
) -> str:
    includes.add("LibJS/Runtime/Value.h")
    includes.add("LibWeb/WebIDL/AbstractOperations.h")
    cpp_type_name = cpp_type_name_for_string(type_name, extended_attributes)
    add_include_for_string_cpp_type(cpp_type_name, includes)
    to_string = "to_utf16_string" if "Utf16" in type_name else "to_string"

    # 1. If V is null and the conversion is to an IDL type associated with the [LegacyNullToEmptyString] extended attribute, then return the DOMString value that represents the empty string.
    if extended_attributes is not None and "LegacyNullToEmptyString" in extended_attributes:
        return f"""[&]() -> JS::ThrowCompletionOr<{cpp_type_name}> {{
        {cpp_type_name} string;
        if (!{value_name}.is_null())
            string = TRY(WebIDL::{to_string}(vm, {value_name}));
        return string;
    }}()"""
    # 2. Let x be ? ToString(V).
    # 3. Return the IDL DOMString value that represents the same sequence of code units as the one the JavaScript String value x represents.
    return f"""[&]() -> JS::ThrowCompletionOr<{cpp_type_name}> {{
        return TRY(WebIDL::{to_string}(vm, {value_name}));
    }}()"""


# 3.2.11. ByteString, https://webidl.spec.whatwg.org/#js-ByteString
def bytestring_to_idl_value(value_name: str, includes: GeneratedIncludes) -> str:
    includes.add("LibJS/Runtime/Value.h")
    includes.add("LibWeb/WebIDL/AbstractOperations.h")
    cpp_type_name = cpp_type_name_for_string("ByteString")
    add_include_for_string_cpp_type(cpp_type_name, includes)
    # 1. Let x be ? ToString(V).
    # 2. If the value of any element of x is greater than 255, then throw a TypeError.
    # 3. Return an IDL ByteString value whose length is the length of x, and where the value of each element is the value of the corresponding element of x.
    return f"""[&]() -> JS::ThrowCompletionOr<{cpp_type_name}> {{
        return TRY(WebIDL::to_byte_string(vm, {value_name}));
    }}()"""


# 3.2.12. USVString, https://webidl.spec.whatwg.org/#js-USVString
def usv_string_to_idl_value(
    value_name: str,
    includes: GeneratedIncludes,
    extended_attributes: Optional[dict[str, str]] = None,
    type_name: str = "USVString",
) -> str:
    includes.add("LibJS/Runtime/Value.h")
    includes.add("LibWeb/WebIDL/AbstractOperations.h")
    cpp_type_name = cpp_type_name_for_string(type_name, extended_attributes)
    add_include_for_string_cpp_type(cpp_type_name, includes)
    to_string = "to_utf16_usv_string" if "Utf16" in type_name else "to_usv_string"

    # 1. Let string be the result of converting V to a DOMString.
    # 2. If x contains any lone surrogates, then throw a TypeError.
    # 3. Return the IDL USVString value that represents the same sequence of code units as the one the JavaScript String value x represents.
    return f"""[&]() -> JS::ThrowCompletionOr<{cpp_type_name}> {{
        return TRY(WebIDL::{to_string}(vm, {value_name}));
    }}()"""


# 3.2.13. object, https://webidl.spec.whatwg.org/#js-object
def object_to_idl_value(value_name: str, includes: GeneratedIncludes) -> str:
    includes.add("LibJS/Runtime/Error.h")
    includes.add("LibJS/Runtime/Object.h")
    includes.add("LibJS/Runtime/Value.h")
    includes.add("LibJS/Runtime/ValueInlines.h")
    # 1. If V is not an Object, then throw a TypeError.
    # 2. Return the IDL object value that is a reference to the same object as V.
    return f"""[&]() -> JS::ThrowCompletionOr<GC::Ref<JS::Object>> {{
        if (!{value_name}.is_object())
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObject, {value_name});
        return GC::Ref {{ {value_name}.as_object() }};
    }}()"""


# 3.2.14. symbol, https://webidl.spec.whatwg.org/#js-symbol
def symbol_to_idl_value(value_name: str, includes: GeneratedIncludes) -> str:
    includes.add("LibJS/Runtime/Value.h")
    # 1. If V is not a Symbol, then throw a TypeError.
    # 2. Return the result of converting V to an IDL symbol value.
    raise RuntimeError("symbol to IDL value conversion is not yet implemented")


# 3.2.15. Interface types, https://webidl.spec.whatwg.org/#js-interface
def interface_to_idl_value(
    value_name: str,
    includes: GeneratedIncludes,
    interface_like_type: InterfaceLikeType,
) -> str:
    includes.add("LibJS/Runtime/Error.h")
    includes.add("LibJS/Runtime/Value.h")
    includes.add("LibJS/Runtime/ValueInlines.h")
    includes.add(interface_like_type.implementation_header)

    # 1. If V implements I, then return the IDL interface type value that represents a reference to that platform object.
    # 2. Throw a TypeError.
    return f"""[&]() -> JS::ThrowCompletionOr<GC::Ref<{interface_like_type.fully_qualified_name}>> {{
        if (auto impl = {value_name}.as_if<{interface_like_type.fully_qualified_name}>())
            return *impl;
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "{interface_like_type.name}");
    }}()"""


# 3.2.16. Callback interface types, https://webidl.spec.whatwg.org/#js-callback-interface
def callback_interface_to_idl_value(
    value_name: str,
    includes: GeneratedIncludes,
    interface: Interface,
) -> str:
    includes.add("LibGC/Heap.h")
    includes.add("LibJS/Runtime/Error.h")
    includes.add("LibJS/Runtime/Value.h")
    includes.add("LibJS/Runtime/ValueInlines.h")
    includes.add("LibWeb/Bindings/ExceptionOrUtils.h")
    includes.add("LibWeb/HTML/Scripting/Environments.h")
    includes.add("LibWeb/WebIDL/CallbackType.h")
    includes.add(implementation_header_for_interface(interface))

    cpp_type = fully_qualified_name_for_interface(interface)
    # 1. If V is not an Object, then throw a TypeError.
    # 2. Return the IDL callback interface type value that represents a reference to V, with the incumbent settings object as the callback context.
    return f"""[&]() -> JS::ThrowCompletionOr<GC::Ref<{cpp_type}>> {{
        if (!{value_name}.is_object())
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObject, {value_name});

        auto callback_type = vm.heap().allocate<WebIDL::CallbackType>({value_name}.as_object(), HTML::incumbent_realm());
        return TRY(throw_dom_exception_if_needed(vm, [&] {{ return {cpp_type}::create(realm, callback_type); }}));
    }}()"""


# 3.2.17. Dictionary types, https://webidl.spec.whatwg.org/#js-dictionary
def dictionary_to_idl_value(
    idl_type: IDLType,
    value_name: str,
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> str:
    add_binding_include_for_type(idl_type, includes, context)
    # The actual implementation of this conversion function is generated by write_dictionary_conversion().
    return f"{converter_function_name(idl_type)}(vm, {value_name})"


# 3.2.18. Enumeration types, https://webidl.spec.whatwg.org/#js-enumeration
def enumeration_to_idl_value(
    idl_type: IDLType,
    value_name: str,
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> str:
    add_binding_include_for_type(idl_type, includes, context)
    # The actual implementation of this conversion function is generated by write_enumeration_conversion().
    return f"{converter_function_name(idl_type)}(vm, {value_name})"


# 3.2.19. Callback function types, https://webidl.spec.whatwg.org/#js-callback-function
def callback_function_to_idl_value(
    return_cpp_type: str,
    callback_function: CallbackFunction,
    value_name: str,
    includes: GeneratedIncludes,
) -> str:
    includes.add("LibGC/Heap.h")
    includes.add("LibJS/Runtime/Error.h")
    includes.add("LibJS/Runtime/FunctionObject.h")
    includes.add("LibWeb/HTML/Scripting/Environments.h")
    includes.add("LibWeb/WebIDL/CallbackType.h")

    operation_returns_promise = (
        "WebIDL::OperationReturnsPromise::Yes"
        if callback_function.return_type.name.split("<", 1)[0] == "Promise"
        else "WebIDL::OperationReturnsPromise::No"
    )

    legacy_treat_non_object_as_null = "LegacyTreatNonObjectAsNull" in callback_function.extended_attributes
    return_cpp_type = "GC::Ptr<WebIDL::CallbackType>" if legacy_treat_non_object_as_null else return_cpp_type

    conversion = f"""[&]() -> JS::ThrowCompletionOr<{return_cpp_type}> {{
"""

    # 1. If the result of calling IsCallable(V) is false and the conversion to an IDL value is not being
    #    performed due to V being assigned to an attribute whose type is a nullable callback function that
    #    is annotated with [LegacyTreatNonObjectAsNull], then throw a TypeError.
    if not legacy_treat_non_object_as_null:
        conversion += f"""
        if (!{value_name}.is_function())
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAFunction, {value_name});
"""

    # 2. Return the IDL callback function type value that represents a reference to the same object that V
    #    represents, with the incumbent settings object as the callback context.
    conversion += f"""
        return vm.heap().allocate<WebIDL::CallbackType>({value_name}.as_object(),  HTML::incumbent_realm(), {operation_returns_promise});
    }}()"""
    return conversion


# 3.2.20. Nullable types — T?, https://webidl.spec.whatwg.org/#js-nullable-type
def nullable_to_idl_value(
    idl_type: IDLType,
    identifier: str,
    extended_attributes: dict[str, str],
    return_cpp_type: str,
    value_name: str,
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> str:
    inner_type = idl_type.clone_with_nullable(False)
    cpp_type = cpp_type_for_idl_type_details(
        idl_type,
        context,
        extended_attributes=extended_attributes,
    )
    conversion = f"""[&]() -> JS::ThrowCompletionOr<{return_cpp_type}> {{
        {cpp_type.name} value;
"""

    # 1. If V is not an Object, and the conversion to an IDL value is being performed due to V being assigned to an
    #    attribute whose type is a nullable callback function that is annotated with [LegacyTreatNonObjectAsNull],
    #    then return the IDL nullable type T? value null.
    callback_function = context.callback_function(inner_type)
    legacy_treat_non_object_as_null = (
        callback_function is not None and "LegacyTreatNonObjectAsNull" in callback_function.extended_attributes
    )
    if legacy_treat_non_object_as_null:
        conversion += f"        if ({value_name}.is_object()) {{\n"

    inner_conversion = to_idl_value_from_type(
        inner_type, identifier, extended_attributes, value_name, includes, context
    )
    inner_return = (
        f"TRY({inner_conversion})" if idl_value_conversion_is_throwing(inner_type, context) else inner_conversion
    )
    inner_type_includes_undefined = inner_type.includes_undefined()

    # 2. Otherwise, if V is undefined, and T includes undefined, return the unique undefined value.
    if inner_type_includes_undefined:
        conversion += f"""
        if ({value_name}.is_undefined()) {{
            value = Empty {{}};
        }}
"""
        # 3. Otherwise, if V is null or undefined, then return the IDL nullable type T? value null.
        conversion += f"""
        else if (!{value_name}.is_nullish()) {{
"""
    else:
        # 3. Otherwise, if V is null or undefined, then return the IDL nullable type T? value null.
        conversion += f"""
        if (!{value_name}.is_nullish()) {{
"""

    # 4. Otherwise, return the result of converting V using the rules for the inner IDL type T.
    conversion += f"""
            value = {inner_return};
        }}
"""

    if legacy_treat_non_object_as_null:
        conversion += "        }\n"

    conversion += """        return value;
    }()"""
    return conversion


# 3.2.21. Sequences — sequence<T>, https://webidl.spec.whatwg.org/#js-sequence
def sequence_to_idl_value(
    sequence_type: IDLParameterizedType,
    identifier: str,
    value_name: str,
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> str:
    element_type = sequence_type.parameters[0]
    element_cpp_type = cpp_type_for_idl_type_details(element_type, context)
    storage_type_name = element_cpp_type.contained_storage_type.value

    # 1. If V is not an Object, throw a TypeError.
    # 2. Let method be ? GetMethod(V, %Symbol.iterator%).
    # 3. If method is undefined, throw a TypeError.
    # 4. Return the result of creating a sequence from V and method.
    return f"""[&]() -> JS::ThrowCompletionOr<{storage_type_name}<{element_cpp_type.name}>> {{
        if (!{value_name}.is_object())
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObject, {value_name});

        auto method = TRY({value_name}.get_method(vm, vm.well_known_symbol_iterator()));
        if (!method)
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotIterable, {value_name});

        return TRY({create_sequence_from_iterable(sequence_type, identifier, value_name, "method", includes, context)});
    }}()"""


# 3.2.21.1. Creating a sequence from an iterable, https://webidl.spec.whatwg.org/#create-sequence-from-iterable
def create_sequence_from_iterable(
    sequence_type: IDLParameterizedType,
    identifier: str,
    value_name: str,
    iterator_method_name: str,
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> str:
    includes.add("LibJS/Runtime/Error.h")
    includes.add("LibJS/Runtime/Iterator.h")
    includes.add("LibJS/Runtime/Value.h")
    includes.add("LibJS/Runtime/ValueInlines.h")
    includes.add("LibWeb/Bindings/ExceptionOrUtils.h")

    element_type = sequence_type.parameters[0]
    element_cpp_type = cpp_type_for_idl_type_details(element_type, context)
    storage_type_name = element_cpp_type.contained_storage_type.value

    return f"""[&]() -> JS::ThrowCompletionOr<{storage_type_name}<{element_cpp_type.name}>> {{
        // To create an IDL value of type sequence<T> given an iterable iterable and an iterator getter method, perform the following steps:
        // 1. Let iteratorRecord be ? GetIteratorFromMethod(iterable, method).
        auto iterator = TRY(JS::get_iterator_from_method(vm, {value_name}, *{iterator_method_name}));

        {storage_type_name}<{element_cpp_type.name}> sequence;

        // 2. Initialize i to be 0.
        // 3. Repeat
        for (;;) {{
            // 1. Let next be ? IteratorStepValue(iteratorRecord).
            auto next = TRY(JS::iterator_step_value(vm, iterator));

            // 2. If next is done, then return an IDL sequence value of type sequence<T> of length i, where the value of the element at index j is Sj.
            if (!next.has_value())
                break;

            // 3. Initialize Si to the result of converting next to an IDL value of type T.
            auto next_value = next.release_value();
            auto sequence_item = TRY(throw_dom_exception_if_needed(vm, [&] {{ return {to_idl_value_from_type(element_type, identifier, {}, "next_value", includes, context)}; }}));

            // 4. Set i to i + 1.
            sequence.append(sequence_item);
        }}

        return sequence;
    }}()"""


# 3.2.22. Async sequences — async_sequence<T>, https://webidl.spec.whatwg.org/#js-async-iterable
def async_sequence_to_idl_value() -> NoReturn:
    raise RuntimeError("async sequence to IDL value conversion is not yet implemented")


# 3.2.23. Records — record<K, V>, https://webidl.spec.whatwg.org/#js-record
def record_to_idl_value(
    record_type: IDLParameterizedType,
    identifier: str,
    value_name: str,
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> str:
    if len(record_type.parameters) != 2:
        raise RuntimeError("Record type must have exactly two parameters")

    key_type = record_type.parameters[0]
    value_type = record_type.parameters[1]
    if not is_string_type(key_type.name):
        raise RuntimeError("Record key type must be a string type")

    includes.add("LibJS/Runtime/Error.h")
    includes.add("LibJS/Runtime/PropertyKey.h")
    includes.add("LibJS/Runtime/Value.h")
    includes.add("LibJS/Runtime/ValueInlines.h")

    record_cpp_type = cpp_type_for_idl_type(record_type, context)

    return f"""[&]() -> JS::ThrowCompletionOr<{record_cpp_type}> {{
        // An ECMAScript value O is converted to an IDL record<K, V> value as follows:
        // 1. If Type(O) is not Object, throw a TypeError.
        if (!{value_name}.is_object())
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObject, {value_name});

        auto& record_object0 = {value_name}.as_object();

        // 2. Let result be a new empty instance of record<K, V>.
        {record_cpp_type} record;

        // 3. Let keys be ? O.[[OwnPropertyKeys]]().
        auto keys = TRY(record_object0.internal_own_property_keys());

        // 4. For each key of keys:
        for (auto& key : keys) {{
            auto property_key = MUST(JS::PropertyKey::from_value(vm, key));

            // 1. Let desc be ? O.[[GetOwnProperty]](key).
            auto desc = TRY(record_object0.internal_get_own_property(property_key));

            // 2. If desc is not undefined and desc.[[Enumerable]] is true:
            if (!desc.has_value() || !desc->enumerable.has_value() || !desc->enumerable.value())
                continue;

            // 1. Let typedKey be key converted to an IDL value of type K.
            auto typed_key = TRY({to_idl_value_from_type(key_type, identifier, {}, "key", includes, context)});

            // 2. Let value be ? Get(O, key).
            auto value = TRY(record_object0.get(property_key));

            // 3. Let typedValue be value converted to an IDL value of type V.
            auto typed_value = TRY({to_idl_value_from_type(value_type, identifier, {}, "value", includes, context)});

            // 4. Set result[typedKey] to typedValue.
            record.set(typed_key, typed_value);
        }}

        // 5. Return result.
        return record;
    }}()"""


# 3.2.24. Promise types — Promise<T>, https://webidl.spec.whatwg.org/#js-promise
def promise_to_idl_value(
    value_name: str,
    includes: GeneratedIncludes,
) -> str:
    includes.add("LibJS/Runtime/AbstractOperations.h")
    includes.add("LibJS/Runtime/PromiseConstructor.h")

    # An ECMAScript value V is converted to an IDL Promise<T> value as follows:
    return f"""[&]() -> JS::ThrowCompletionOr<GC::Ref<WebIDL::Promise>> {{
        auto& realm = *vm.current_realm();

        // 1. Let promiseCapability be ? NewPromiseCapability(%Promise%).
        auto promise_capability0 = TRY(JS::new_promise_capability(vm, realm.intrinsics().promise_constructor()));

        // 2. Perform ? Call(promiseCapability.[[Resolve]], undefined, « V »).
        TRY(JS::call(vm, *promise_capability0->resolve(), JS::js_undefined(), {value_name}));

        // 3. Return promiseCapability.
        return promise_capability0;
    }}()"""


# 3.2.25. Union types, https://webidl.spec.whatwg.org/#js-union
def union_to_idl_value(
    union_type: IDLUnionType,
    identifier: str,
    extended_attributes: dict[str, str],
    value_name: str,
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> str:
    # https://webidl.spec.whatwg.org/#es-union
    includes.add("AK/Variant.h")

    variant_type = union_type_to_variant(union_type, context)
    types = collect_union_types(union_type.flattened_member_types(), context)
    sections: list[str] = []

    def conversion_for(member_type: IDLType, name: str = value_name) -> str:
        # Null is handled at the union level and stored as Empty in the generated Variant.
        return to_idl_value_from_type(
            member_type.clone_with_nullable(False),
            identifier,
            extended_attributes,
            name,
            includes,
            context,
        )

    def append(section: str) -> None:
        if section:
            sections.append(section)

    # 1. If the union type includes undefined and V is undefined, then return the unique undefined value.
    if union_type.includes_undefined():
        includes.add("AK/Types.h")
        append(f"""
        if ({value_name}.is_undefined()) {{
            return Empty {{}};
        }}
""")

    # 2. If the union type includes a nullable type and V is null or undefined, then return the IDL value null.
    if union_type.includes_nullable_type():
        includes.add("AK/Types.h")
        append(f"""
        if ({value_name}.is_nullish()) {{
            return Empty {{}};
        }}
""")

    # 4. If V is null or undefined, then:
    #     1. If types includes a dictionary type, then return the result of converting V to that dictionary type.
    if not union_type.includes_nullable_type() and types.dictionary_type is not None:
        append(f"""
        if ({value_name}.is_nullish()) {{
            return {variant_type} {{ TRY({dictionary_to_idl_value(types.dictionary_type, value_name, includes, context)}) }};
        }}
""")

    # NB: Doing this here simplifies logic below.
    append(f"""
        if ({value_name}.is_object()) {{
            [[maybe_unused]] auto& object = {value_name}.as_object();
""")

    # 5. If V is a platform object, then:
    if types.interface_types:
        includes.add("LibWeb/Bindings/PlatformObject.h")
        append("""
            if (is<PlatformObject>(object)) {
""")
        for interface_type in types.interface_types:
            interface_like_type = interface_like_type_for_idl_type(interface_type, context)
            assert interface_like_type is not None
            platform_object_cpp_type = interface_like_type.fully_qualified_name
            includes.add(interface_like_type.implementation_header)
            # 1. If types includes an interface type that V implements, then return the IDL value that is a reference to the object V.
            append(f"""
                if (auto* result = as_if<{platform_object_cpp_type}>(object))
                    return {variant_type} {{ GC::Ref {{ *result }} }};
""")
        # 2. If types includes object, then return the IDL value that is a reference to the object V.
        if types.includes_object_type:
            append(f"""                return {variant_type} {{ GC::Ref {{ object }} }};
""")
        append("""            }
""")

    # 6. If Type(V) is Object, V has an [[ArrayBufferData]] internal slot, and IsSharedArrayBuffer(V) is false, then:
    if types.array_buffer_type is not None:
        includes.add("LibJS/Runtime/ArrayBuffer.h")
        array_buffer_conversion = conversion_for(types.array_buffer_type)
        # 1. If types includes ArrayBuffer, then return the result of converting V to ArrayBuffer.
        # 2. If types includes object, then return the IDL value that is a reference to the object V.
        append(f"""
            if (auto* array_buffer = as_if<JS::ArrayBuffer>(object)) {{
                if (!array_buffer->is_shared_array_buffer()) {{
                    // 1. If types includes ArrayBuffer, then return the result of converting V to ArrayBuffer.
                    auto array_buffer_union_type = TRY({array_buffer_conversion});
                    return {variant_type} {{ array_buffer_union_type }};
                }}
            }}
""")
    # FIXME: 7. If V is an Object, V, has an [[ArrayBufferData]] internal slot, and IsSharedArrayBuffer(V) is true, then:

    # 8. If V is an Object and V has a [[DataView]] internal slot, then:
    if types.data_view_type is not None:
        includes.add("LibJS/Runtime/DataView.h")
        data_view_conversion = conversion_for(types.data_view_type)
        # 1. If types includes DataView, then return the result of converting V to DataView.
        # 2. If types includes object, then return the IDL value that is a reference to the object V.
        append(f"""
            if (as_if<JS::DataView>(object)) {{
                // 1. If types includes DataView, then return the result of converting V to DataView.
                auto data_view_union_type = TRY({data_view_conversion});
                return {variant_type} {{ data_view_union_type }};
            }}
""")

    # 9. If V is an Object and V has a [[TypedArrayName]] internal slot, then:
    if types.typed_array_types:
        includes.add("LibJS/Runtime/TypedArray.h")
        for typed_array_type in types.typed_array_types:
            typed_array_conversion = conversion_for(typed_array_type)
            typed_array_cpp_name = make_name_acceptable_cpp(title_case_to_snake_case(typed_array_type.name))
            # 1. If types includes a typed array type whose name is the value of V’s [[TypedArrayName]] internal slot, then return the result of converting V to that type.
            # 2. If types includes object, then return the IDL value that is a reference to the object V.
            append(f"""
            if (as_if<JS::{typed_array_type.name}>(object)) {{
                auto {typed_array_cpp_name}_union_type = TRY({typed_array_conversion});
                return {variant_type} {{ {typed_array_cpp_name}_union_type }};
            }}
""")

    # 1. If IsCallable(V) is true, then:
    if types.callback_function_types:
        callback_function_type = types.callback_function_types[0]
        callback_function = context.callback_function(callback_function_type)
        assert callback_function is not None
        # 1. If types includes a callback function type, then return the result of converting V to that callback function type.
        # 2. If types includes object, then return the IDL value that is a reference to the object V.
        callback_function_conversion = callback_function_to_idl_value(
            cpp_type_for_idl_type(callback_function_type, context, extended_attributes=extended_attributes),
            callback_function,
            value_name,
            includes,
        )
        append(f"""
            if (object.is_function()) {{
                auto callback_function_union_type = TRY({callback_function_conversion});
                return {variant_type} {{ callback_function_union_type }};
            }}

""")

    # 11. If V is an Object, then:
    # FIXME: 11.1 If types includes an async sequence type, then
    # 11.2. If types includes a sequence type, then
    if types.sequence_type is not None:
        append(f"""
            // 1. Let method be ? GetMethod(V, @@iterator).
            auto method = TRY({value_name}.get_method(vm, vm.well_known_symbol_iterator()));

            // 2. If method is not undefined, return the result of creating a sequence of that type from V and method.
            if (method) {{
                auto sequence_union_type = TRY({create_sequence_from_iterable(types.sequence_type, identifier, value_name, "method", includes, context)});
                return {variant_type} {{ sequence_union_type }};
            }}

""")
    # 11.3. If types includes a frozen array type, then
    if types.frozen_array_type is not None:
        append(f"""
            // 1. Let method be ? GetMethod(V, @@iterator).
            auto frozen_array_method = TRY({value_name}.get_method(vm, vm.well_known_symbol_iterator()));

            // 2. If method is not undefined, return the result of creating a frozen array of that type from V and method.
            if (frozen_array_method) {{
                auto frozen_array_union_type = TRY({create_sequence_from_iterable(types.frozen_array_type, identifier, value_name, "frozen_array_method", includes, context)});
                return {variant_type} {{ frozen_array_union_type }};
            }}
""")

    # 11.4. If types includes a dictionary type, then return the result of converting V to that dictionary type.
    if types.dictionary_type is not None:
        dictionary_conversion = dictionary_to_idl_value(types.dictionary_type, value_name, includes, context)
        append(f"            return {variant_type} {{ TRY({dictionary_conversion}) }};\n")

    # 11.5. If types includes a record type, then return the result of converting V to that record type.
    if types.record_type is not None:
        record_conversion = record_to_idl_value(types.record_type, identifier, value_name, includes, context)
        append(f"""
            auto record_union_type = TRY({record_conversion});
            return {variant_type} {{ record_union_type }};
""")

    # 11.6. If types includes a callback interface type, then return the result of converting V to that callback interface type.
    if types.callback_interface_types:
        callback_interface_type = types.callback_interface_types[0]
        callback_interface = context.interface(callback_interface_type)
        assert callback_interface is not None
        callback_interface_conversion = callback_interface_to_idl_value(
            value_name,
            includes,
            callback_interface,
        )
        append(f"""
            auto callback_interface_union_type = TRY({callback_interface_conversion});
            return {variant_type} {{ callback_interface_union_type }};
""")

    # 11.7. If types includes object, then return the IDL value that is a reference to the object V.
    if types.includes_object_type:
        append(f"""
            return {variant_type} {{ GC::Ref {{ object }} }};
""")

    append("        }\n")

    # 12. If Type(V) is Boolean, then:
    if types.boolean_type is not None:
        # 1. If types includes boolean, then return the result of converting V to boolean.
        append(f"""
        if ({value_name}.is_boolean())
            return {variant_type} {{ {value_name}.as_bool() }};
""")

    # 13. If V is a Number, then:
    if types.numeric_type is not None:
        # 1. If types includes a numeric type, then return the result of converting V to that numeric type.
        numeric_value_name = f"{make_name_acceptable_cpp(identifier)}_number"
        numeric_conversion_expression = conversion_for(types.numeric_type)
        append(f"""
        if ({value_name}.is_number()) {{
            auto {numeric_value_name} = TRY({numeric_conversion_expression});
            return {variant_type} {{ {numeric_value_name} }};
        }}
""")

    # 14. If V is a BigInt, then:
    if types.bigint_type is not None:
        includes.add("LibJS/Runtime/BigInt.h")
        # 1. If types includes bigint, then return the result of converting V to bigint
        append(f"""
        if ({value_name}.is_bigint())
            return {variant_type} {{ GC::Ref {{ {value_name}.as_bigint() }} }};
""")

    # 15. If types includes a string type, then return the result of converting V to that type.
    # NB: We need to special case enumurations since we don't pass those through as strings.
    # FIXME: Can we use a helper here?
    if types.enumeration_types:
        includes.add("AK/StringView.h")
        includes.add("LibJS/Runtime/ValueInlines.h")
        enumeration_conversion = f"""
        if ({value_name}.is_string()) {{
            auto enumeration_string = TRY({value_name}.to_string(vm));
"""
        for enumeration_type in types.enumeration_types:
            enumeration = context.enumeration(enumeration_type)
            assert enumeration is not None
            for enumeration_value in enumeration.values:
                enumeration_conversion += f"""            if (enumeration_string == "{enumeration_value}"sv)
                return {variant_type} {{ {enumeration.name}::{string_to_cpp_enum_name(enumeration_value)} }};
"""
        enumeration_conversion += """        }

"""
        append(enumeration_conversion)

    # 15. If types includes a string type, then return the result of converting V to that type.
    if types.string_type is not None:
        string_value_name = f"{make_name_acceptable_cpp(identifier)}_string"
        string_conversion_expression = conversion_for(types.string_type)
        append(f"""
        auto {string_value_name} = TRY({string_conversion_expression});
        return {variant_type} {{ {string_value_name} }};
""")

    # 16. If types includes a numeric type and bigint, then return the result of converting V to either that numeric type or bigint.
    if types.numeric_type is not None and types.bigint_type is not None:
        includes.add("LibJS/Runtime/BigInt.h")
        includes.add("LibJS/Runtime/ValueInlines.h")
        numeric_value_name = f"{make_name_acceptable_cpp(identifier)}_number"
        numeric_conversion_expression = conversion_for(types.numeric_type, "x")
        # https://webidl.spec.whatwg.org/#js-bigint
        append(f"""
        // An ECMAScript value V is converted to an IDL numeric type T or bigint value by running the following algorithm:
        // 1. Let x be ? ToNumeric(V).
        auto x = TRY({value_name}.to_numeric(vm));

        // 2. If Type(x) is BigInt, then:
        if (x.is_bigint())
            // 1. Return the IDL bigint value that represents the same numeric value as x.
            return {variant_type} {{ GC::Ref {{ x.as_bigint() }} }};

        // 3. Assert: Type(x) is Number.
        VERIFY(x.is_number());

        // 4. Return the result of converting x to T.
        auto {numeric_value_name}_fallback = TRY({numeric_conversion_expression});
        return {variant_type} {{ {numeric_value_name}_fallback }};
""")

    # 17. If types includes a numeric type, then return the result of converting V to that numeric type.
    if types.numeric_type is not None:
        numeric_value_name = f"{make_name_acceptable_cpp(identifier)}_number"
        numeric_conversion_expression = conversion_for(types.numeric_type)
        append(f"""
        auto {numeric_value_name}_fallback = TRY({numeric_conversion_expression});
        return {variant_type} {{ {numeric_value_name}_fallback }};
""")

    # 18. If types includes boolean, then return the result of converting V to boolean.
    if types.boolean_type is not None:
        append(f"""
        return {variant_type} {{ {value_name}.to_boolean() }};
""")

    # 19. If types includes bigint, then return the result of converting V to bigint.
    if types.bigint_type is not None:
        append(
            f"""
        auto bigint_fallback = TRY({bigint_to_idl_value(value_name, includes)});
        return {variant_type} {{ bigint_fallback }};
"""
        )

    # 20. Throw a TypeError.
    includes.add("LibJS/Runtime/Error.h")
    append("""
        return vm.throw_completion<JS::TypeError>("No union types matched"sv);
""")

    return f"""[&]() -> JS::ThrowCompletionOr<{variant_type}> {{
{"".join(sections)}    }}()"""


def buffer_source_to_idl_value(
    idl_type: IDLType,
    extended_attributes: dict[str, str],
    value_cpp_type_name: str,
    value_name: str,
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> str:
    type_name = idl_type.name

    includes.add("AK/TypeCasts.h")
    includes.add("LibJS/Runtime/Error.h")
    includes.add("LibJS/Runtime/Value.h")
    add_buffer_source_type_include(idl_type, includes)

    def array_buffer_checks(buffer_name: str) -> str:
        allow_shared_check = ""
        if "AllowShared" not in extended_attributes:
            allow_shared_check = f"""        // 2. If IsSharedArrayBuffer(V) is true, then throw a TypeError.
        if ({buffer_name}->is_shared_array_buffer())
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::SharedArrayBuffer);

"""

        allow_resizable_check = ""
        if "AllowResizable" not in extended_attributes:
            allow_resizable_check = f"""        // 3. If the conversion is not to an IDL type associated with the [AllowResizable] extended attribute, and
        //    IsFixedLengthArrayBuffer(V) is false, then throw a TypeError.
        if (!{buffer_name}->is_fixed_length())
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "fixed-length {type_name}");

"""

        return f"{allow_shared_check}{allow_resizable_check}"

    def array_buffer_view_checks(buffer_name: str) -> str:
        allow_shared_check = ""
        if "AllowShared" not in extended_attributes:
            allow_shared_check = f"""        // 2. If the conversion is not to an IDL type associated with the [AllowShared] extended attribute, and
        //    IsSharedArrayBuffer(V.[[ViewedArrayBuffer]]) is true, then throw a TypeError.
        if ({buffer_name}.is_shared_array_buffer())
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::SharedArrayBuffer);

"""

        allow_resizable_check = ""
        if "AllowResizable" not in extended_attributes:
            allow_resizable_check = f"""        // 3. If the conversion is not to an IDL type associated with the [AllowResizable] extended attribute, and
        //    IsFixedLengthArrayBuffer(V.[[ViewedArrayBuffer]]) is false, then throw a TypeError.
        if (!{buffer_name}.is_fixed_length())
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "fixed-length {type_name}");

"""

        return f"{allow_shared_check}{allow_resizable_check}"

    if type_name == "ArrayBuffer":
        return f"""[&]() -> JS::ThrowCompletionOr<{value_cpp_type_name}> {{
        // A JavaScript value V is converted to an IDL ArrayBuffer value by running the following algorithm:
        // 1. If V is not an Object, or V does not have an [[ArrayBufferData]] internal slot, then throw a TypeError.
        auto builtin_buffer = {value_name}.as_if<JS::ArrayBuffer>();
        if (!builtin_buffer)
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "{type_name}");

{array_buffer_checks("builtin_buffer")}        // 4. Return the IDL ArrayBuffer value that is a reference to the same object as V.
        return GC::Ref {{ *builtin_buffer }};
    }}()"""

    if type_name == "DataView":
        includes.add("LibJS/Runtime/DataView.h")
        return f"""[&]() -> JS::ThrowCompletionOr<{value_cpp_type_name}> {{
        // A JavaScript value V is converted to an IDL DataView value by running the following algorithm:
        // 1. If V is not an Object, or V does not have a [[DataView]] internal slot, then throw a TypeError.
        auto builtin_buffer = {value_name}.as_if<JS::DataView>();
        if (!builtin_buffer)
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "{type_name}");

        auto& viewed_array_buffer = *builtin_buffer->viewed_array_buffer();

{array_buffer_view_checks("viewed_array_buffer")}        // 4. Return the IDL DataView value that is a reference to the same object as V.
        return GC::Ref {{ *builtin_buffer }};
    }}()"""

    if is_typed_array_type(idl_type):
        includes.add("LibJS/Runtime/TypedArray.h")
        return f"""[&]() -> JS::ThrowCompletionOr<{value_cpp_type_name}> {{
        // A JavaScript value V is converted to an IDL typed array value by running the following algorithm:
        // 1. Let T be the IDL type V is being converted to.
        // 2. If V is not an Object, or V does not have a [[TypedArrayName]] internal slot with a value equal to T's name,
        //    then throw a TypeError.
        auto builtin_buffer = {value_name}.as_if<JS::{type_name}>();
        if (!builtin_buffer)
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "{type_name}");

        auto& viewed_array_buffer = *builtin_buffer->viewed_array_buffer();

{array_buffer_view_checks("viewed_array_buffer")}        // 5. Return the IDL value of type T that is a reference to the same object as V.
        return GC::Ref {{ *builtin_buffer }};
    }}()"""

    return unsupported_to_idl_value(idl_type)


def to_idl_value_from_type(
    idl_type: IDLType,
    identifier: str,
    extended_attributes: dict[str, str],
    value_name: str,
    includes: GeneratedIncludes,
    context: GenerationContext,
    return_cpp_type: Optional[str] = None,
    value_cpp_type_name: Optional[str] = None,
) -> str:
    type_name = idl_type.name
    return_cpp_type = return_cpp_type or cpp_type_for_idl_type(
        idl_type, context, extended_attributes=extended_attributes
    )
    value_cpp_type_name = value_cpp_type_name or return_cpp_type

    if idl_type.nullable:
        return nullable_to_idl_value(
            idl_type, identifier, extended_attributes, return_cpp_type, value_name, includes, context
        )

    if type_name == "any":
        return any_to_idl_value(value_name, includes)
    if type_name == "boolean":
        return boolean_to_idl_value(value_name, includes)
    if type_name == "byte":
        return byte_to_idl_value(value_name, includes, extended_attributes)
    if type_name == "octet":
        return octet_to_idl_value(value_name, includes, extended_attributes)
    if type_name == "short":
        return short_to_idl_value(value_name, includes, extended_attributes)
    if type_name == "unsigned short":
        return unsigned_short_to_idl_value(value_name, includes, extended_attributes)
    if type_name == "long":
        return long_to_idl_value(value_name, includes, extended_attributes)
    if type_name == "unsigned long":
        return unsigned_long_to_idl_value(value_name, includes, extended_attributes)
    if type_name == "long long":
        return long_long_to_idl_value(value_name, includes, extended_attributes)
    if type_name == "unsigned long long":
        return unsigned_long_long_to_idl_value(value_name, includes, extended_attributes)
    if type_name == "float":
        return float_to_idl_value(value_name, includes, identifier)
    if type_name == "unrestricted float":
        return unrestricted_float_to_idl_value(value_name, includes)
    if type_name == "double":
        return double_to_idl_value(value_name, includes, identifier)
    if type_name == "unrestricted double":
        return unrestricted_double_to_idl_value(value_name, includes)
    if type_name == "bigint":
        return bigint_to_idl_value(value_name, includes)
    if type_name in ("DOMString", "Utf16DOMString"):
        return dom_string_to_idl_value(
            value_name,
            includes,
            extended_attributes,
            type_name,
        )
    if type_name == "ByteString":
        return bytestring_to_idl_value(value_name, includes)
    if type_name in ("USVString", "Utf16USVString"):
        return usv_string_to_idl_value(
            value_name,
            includes,
            extended_attributes,
            type_name,
        )
    if type_name == "object":
        return object_to_idl_value(value_name, includes)
    if type_name == "symbol":
        return symbol_to_idl_value(value_name, includes)

    interface_like_type = interface_like_type_for_idl_type(idl_type, context)
    if interface_like_type is not None:
        return interface_to_idl_value(value_name, includes, interface_like_type)

    interface = context.interface(idl_type)
    if interface is not None and interface.is_callback_interface:
        return callback_interface_to_idl_value(value_name, includes, interface)

    if context.dictionary(idl_type) is not None:
        return dictionary_to_idl_value(idl_type, value_name, includes, context)

    if context.enumeration(idl_type) is not None:
        return enumeration_to_idl_value(idl_type, value_name, includes, context)

    callback_function = context.callback_function(idl_type)
    if callback_function is not None:
        return callback_function_to_idl_value(return_cpp_type, callback_function, value_name, includes)

    if isinstance(idl_type, IDLParameterizedType) and type_name == "sequence":
        return sequence_to_idl_value(idl_type, identifier, value_name, includes, context)

    if isinstance(idl_type, IDLParameterizedType) and type_name == "async_sequence":
        return async_sequence_to_idl_value()

    if isinstance(idl_type, IDLParameterizedType) and type_name == "record":
        return record_to_idl_value(idl_type, identifier, value_name, includes, context)

    if isinstance(idl_type, IDLParameterizedType) and type_name == "Promise":
        return promise_to_idl_value(value_name, includes)

    if isinstance(idl_type, IDLUnionType):
        return union_to_idl_value(idl_type, identifier, extended_attributes, value_name, includes, context)

    if is_buffer_source_type(idl_type):
        return buffer_source_to_idl_value(
            idl_type, extended_attributes, value_cpp_type_name, value_name, includes, context
        )

    if isinstance(idl_type, IDLParameterizedType) and type_name == "FrozenArray":
        return sequence_to_idl_value(idl_type, identifier, value_name, includes, context)

    return unsupported_to_idl_value(idl_type)


def to_idl_value(
    member: DictionaryMemberOrAttribute,
    value_name: str,
    includes: GeneratedIncludes,
    context: GenerationContext,
) -> str:
    return to_idl_value_from_type(
        member.type,
        member.name,
        getattr(member, "extended_attributes", {}),
        value_name,
        includes,
        context,
        return_cpp_type=cpp_type(member, context),
        value_cpp_type_name=cpp_value_type(member, context),
    )


def type_check_idl_value(
    idl_type: IDLType,
    value_name: str,
    includes: GeneratedIncludes,
    context: GenerationContext,
    interface_name: str,
) -> str:
    if is_string_type(idl_type.name):
        return f"""    if (!{value_name}.is_string())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "String");
"""

    interface_like_type = interface_like_type_for_idl_type(idl_type, context)
    if interface_like_type is None:
        raise RuntimeError(f"Unsupported IDL value type '{idl_type.name}' on '{interface_name}'")

    includes.add("AK/TypeCasts.h")
    includes.add(interface_like_type.implementation_header)
    return f"""    if (!{value_name}.is<{interface_like_type.fully_qualified_name}>())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "{interface_like_type.fully_qualified_name}");
"""
