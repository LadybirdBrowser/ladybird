# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from typing import TextIO

from Generators.libweb_bindings.cpp_types import converter_function_name
from Generators.libweb_bindings.includes import GeneratedIncludes
from Generators.libweb_bindings.to_js_value import to_value_function_name
from Utils.utils import make_name_acceptable_cpp
from Utils.utils import title_case_to_snake_case
from Utils.webidl_parser import Dictionary


def from_json_function_name(dictionary: Dictionary) -> str:
    return f"convert_json_to_idl_value_for_{make_name_acceptable_cpp(title_case_to_snake_case(dictionary.name))}"


def to_json_function_name(dictionary: Dictionary) -> str:
    return f"{make_name_acceptable_cpp(title_case_to_snake_case(dictionary.name))}_to_json"


def write_dictionary_json_declarations(out: TextIO, dictionary: Dictionary, includes: GeneratedIncludes) -> None:
    if "GenerateFromJSON" in dictionary.extended_attributes:
        includes.add("AK/Utf16View.h")
        out.write(
            f"JS::ThrowCompletionOr<{dictionary.name}> {from_json_function_name(dictionary)}(JS::VM&, Utf16View);\n\n"
        )

    if "GenerateToJSON" in dictionary.extended_attributes:
        if "GenerateToValue" not in dictionary.extended_attributes:
            raise RuntimeError(f"Dictionary '{dictionary.name}' has [GenerateToJSON] but not [GenerateToValue]")
        includes.add("AK/Utf16String.h")
        out.write(
            f"JS::ThrowCompletionOr<Utf16String> {to_json_function_name(dictionary)}(JS::VM&, {dictionary.name} const&);\n\n"
        )


def write_dictionary_json_conversions(out: TextIO, dictionary: Dictionary, includes: GeneratedIncludes) -> None:
    if "GenerateFromJSON" in dictionary.extended_attributes:
        includes.add("LibJS/Runtime/JSONObject.h")
        includes.add("LibWeb/Bindings/NewGlobalObject.h")
        out.write(
            f"""JS::ThrowCompletionOr<{dictionary.name}> {from_json_function_name(dictionary)}(JS::VM& vm, Utf16View json)
{{
    return run_in_a_new_global_object<{dictionary.name}>(vm, [&](JS::Realm&) -> JS::ThrowCompletionOr<{dictionary.name}> {{
        return {converter_function_name(dictionary)}(vm, TRY(JS::JSONObject::parse_json(vm, json)));
    }});
}}

"""
        )

    if "GenerateToJSON" in dictionary.extended_attributes:
        includes.add("LibWeb/Bindings/NewGlobalObject.h")
        out.write(
            f"""JS::ThrowCompletionOr<Utf16String> {to_json_function_name(dictionary)}(JS::VM& vm, {dictionary.name} const& dictionary)
{{
    return serialize_to_json_in_a_new_global_object(vm, [&](JS::Realm& realm) {{
        return {to_value_function_name(dictionary)}(realm, dictionary);
    }});
}}

"""
        )
