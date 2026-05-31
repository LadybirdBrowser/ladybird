# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from io import StringIO
from typing import TextIO

from Generators.libweb_bindings.includes import GeneratedIncludes
from Utils.utils import make_name_acceptable_cpp
from Utils.utils import title_case_to_snake_case
from Utils.utils import title_casify
from Utils.utils import underlying_type_for_enum
from Utils.webidl_parser import Enumeration
from Utils.webidl_parser import Module


def ensure_python_generated_module_is_supported(module: Module) -> None:
    if module.interface is not None or module.dictionaries:
        raise RuntimeError(f"{module.path}: Python bindings generator only supports enum-only modules")
    if not module.enumerations:
        raise RuntimeError(f"{module.path}: Python bindings generator expected at least one enum")


def write_header(out: TextIO, module: Module) -> None:
    ensure_python_generated_module_is_supported(module)

    includes = GeneratedIncludes()
    body = StringIO()

    for enumeration in module.enumerations:
        write_enumeration_declaration(body, enumeration, includes)

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


def write_implementation(out: TextIO, module: Module) -> None:
    ensure_python_generated_module_is_supported(module)

    includes = GeneratedIncludes()
    includes.add_binding(module.path.stem)
    body = StringIO()

    for enumeration in module.enumerations:
        write_enumeration_conversion(body, enumeration, includes)

    includes.write(out)
    out.write("namespace Web::Bindings {\n\n")
    out.write(body.getvalue())
    out.write("} // namespace Web::Bindings\n")


def conversion_function_name(enumeration: Enumeration) -> str:
    return f"convert_to_idl_value_for_{make_name_acceptable_cpp(title_case_to_snake_case(enumeration.name))}"


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
