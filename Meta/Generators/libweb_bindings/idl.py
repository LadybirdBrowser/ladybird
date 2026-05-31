# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from io import StringIO
from typing import TextIO
from typing import Union

from Generators.libweb_bindings import type_conversion
from Generators.libweb_bindings.includes import GeneratedIncludes
from Utils.utils import make_name_acceptable_cpp
from Utils.utils import title_case_to_snake_case
from Utils.utils import title_casify
from Utils.utils import underlying_type_for_enum
from Utils.webidl_parser import Dictionary
from Utils.webidl_parser import Enumeration
from Utils.webidl_parser import Module


def write_header(out: TextIO, module: Module) -> None:
    includes = GeneratedIncludes()
    body = StringIO()
    local_types = {enumeration.name for enumeration in module.enumerations}

    for enumeration in module.enumerations:
        write_enumeration_declaration(body, enumeration, includes)

    for dictionary in module.dictionaries:
        write_dictionary_declaration(body, dictionary, includes, local_types)

    out.write("#pragma once\n\n")
    includes.write(out)
    out.write("namespace Web::Bindings {\n\n")
    out.write(body.getvalue())
    out.write("} // namespace Web::Bindings\n")


def write_enumeration_declaration(out: TextIO, enumeration: Enumeration, includes: GeneratedIncludes) -> None:
    includes.add("AK/String.h")
    includes.add("LibJS/Forward.h")
    includes.add("LibJS/Runtime/Value.h")

    out.write(f"enum class {enumeration.name} : {underlying_type_for_enum(len(enumeration.values))} {{\n")
    for value in enumeration.values:
        out.write(f"    {enum_member_name(value)},\n")
    out.write("};\n\n")
    out.write(
        f"JS::ThrowCompletionOr<{enumeration.name}> {conversion_function_name(enumeration)}(JS::VM&, JS::Value);\n\n"
    )
    out.write(f"String idl_enum_to_string({enumeration.name});\n\n")


def write_dictionary_declaration(
    out: TextIO, dictionary: Dictionary, includes: GeneratedIncludes, local_types: set[str]
) -> None:
    includes.add("LibJS/Forward.h")
    includes.add("LibJS/Runtime/Value.h")

    out.write(f"struct {dictionary.name} {{\n")
    for member in dictionary.members:
        type_conversion.add_header_includes_for_type(member, includes, local_types)

        default_value = ""
        if member.default_value is not None:
            default_value = f" {type_conversion.cpp_default_value_conversion(member)} "
        out.write(f"    {type_conversion.cpp_type(member)} {type_conversion.cpp_name(member)} {{{default_value}}};\n")
    out.write("};\n\n")
    out.write(
        f"JS::ThrowCompletionOr<{dictionary.name}> {conversion_function_name(dictionary)}(JS::VM&, JS::Value);\n\n"
    )


def write_implementation(out: TextIO, module: Module) -> None:
    includes = GeneratedIncludes()
    includes.add_binding(module.path.stem)
    body = StringIO()

    for enumeration in module.enumerations:
        write_enumeration_conversion(body, enumeration, includes)

    for dictionary in module.dictionaries:
        write_dictionary_conversion(body, dictionary, includes)

    includes.write(out)
    out.write("namespace Web::Bindings {\n\n")
    out.write(body.getvalue())
    out.write("} // namespace Web::Bindings\n")


def conversion_function_name(definition: Union[Dictionary, Enumeration]) -> str:
    return f"convert_to_idl_value_for_{make_name_acceptable_cpp(title_case_to_snake_case(definition.name))}"


def enum_member_name(value: str) -> str:
    return title_casify(value)


def write_enumeration_conversion(out: TextIO, enumeration: Enumeration, includes: GeneratedIncludes) -> None:
    includes.add("LibJS/Runtime/Error.h")
    includes.add("LibJS/Runtime/VM.h")
    includes.add("LibJS/Runtime/ValueInlines.h")

    out.write(
        f"""// https://webidl.spec.whatwg.org/#idl-enumeration
JS::ThrowCompletionOr<{enumeration.name}> {conversion_function_name(enumeration)}(JS::VM& vm, JS::Value value)
{{
    // 1. Let S be the result of calling ? ToString(V).
    auto value_as_string = TRY(value.to_string(vm));

    // 2. If S is not one of E’s enumeration values, then throw a TypeError.
    // 3. Return the enumeration value of type E that is equal to S.
"""
    )

    for value in enumeration.values:
        out.write(f'    if (value_as_string == "{value}"sv)\n')
        out.write(f"        return {enumeration.name}::{enum_member_name(value)};\n")
    out.write(
        f"""    return vm.throw_completion<JS::TypeError>(JS::ErrorType::InvalidEnumerationValue, value_as_string, "{enumeration.name}");
}}

// https://webidl.spec.whatwg.org/#idl-enumeration
String idl_enum_to_string({enumeration.name} value)
{{
    // The result of converting an IDL enumeration type value to a JavaScript value is the String value that represents the same sequence of code units as the enumeration value.
    switch (value) {{
"""
    )

    for value in enumeration.values:
        out.write(f"    case {enumeration.name}::{enum_member_name(value)}:\n")
        out.write(f'        return "{value}"_string;\n')

    out.write(
        """    }
    VERIFY_NOT_REACHED();
}

"""
    )


def write_dictionary_conversion(out: TextIO, dictionary: Dictionary, includes: GeneratedIncludes) -> None:
    includes.add("LibJS/Runtime/Error.h")
    includes.add("LibJS/Runtime/VM.h")
    includes.add("LibJS/Runtime/Value.h")
    includes.add("LibJS/Runtime/ValueInlines.h")
    includes.add("LibWeb/Bindings/ExceptionOrUtils.h")

    out.write(f"""// https://webidl.spec.whatwg.org/#es-dictionary
JS::ThrowCompletionOr<{dictionary.name}> {conversion_function_name(dictionary)}(JS::VM& vm, JS::Value js_dict)
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

    # 4.1. For each dictionary member member declared on dictionary, in lexicographical order:
    for member in dictionary.members:
        conversion = type_conversion.to_idl_value(member, "js_member_value", includes)
        out.write(
            f"""        .{type_conversion.cpp_name(member)} = TRY([&]() -> JS::ThrowCompletionOr<{type_conversion.cpp_type(member)}> {{
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
            auto idl_member_value = {type_conversion.cpp_default_value_conversion(member)};

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
            raise RuntimeError(f"Unsupported optional dictionary member '{member.name}' on '{dictionary.name}' without default value")

        out.write("        }()),\n")
    out.write(
        """    };
}

"""
    )
