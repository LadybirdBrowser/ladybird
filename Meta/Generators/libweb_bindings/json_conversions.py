# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from typing import TextIO

from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.cpp_types import converter_function_name
from Generators.libweb_bindings.includes import GeneratedIncludes
from Generators.libweb_bindings.rust_types import UnsupportedByRustBindings
from Generators.libweb_bindings.rust_types import dictionary_holds_sequence_of_cells
from Generators.libweb_bindings.rust_types import rust_dictionary_path
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


def write_rust_dictionary_json_conversions(out: TextIO, context: GenerationContext, dictionary: Dictionary) -> None:
    from_json = "GenerateFromJSON" in dictionary.extended_attributes
    to_json = "GenerateToJSON" in dictionary.extended_attributes
    if not from_json and not to_json:
        return
    if to_json and "GenerateToValue" not in dictionary.extended_attributes:
        raise UnsupportedByRustBindings(
            f"Dictionary '{dictionary.name}' has [GenerateToJSON] but not [GenerateToValue]"
        )
    if dictionary_holds_sequence_of_cells(dictionary, context):
        raise UnsupportedByRustBindings(
            f"The Rust bindings do not convert the dictionary {dictionary.name} to or from JSON"
        )

    out.write(f"\n    impl {rust_dictionary_path(dictionary, context)} {{\n")
    if from_json:
        out.write(
            """        pub(crate) fn from_json(vm: &Vm, json: Utf16View<'_>) -> ThrowCompletionOr<Self> {
            run_in_a_new_global_object(vm, |_| Self::from_js(vm, JSONObject::parse_json(vm, json, None)?))
        }
"""
        )
    if to_json:
        out.write(
            """        pub(crate) fn to_json(self, vm: &Vm) -> ThrowCompletionOr<Utf16String> {
            serialize_to_json_in_a_new_global_object(vm, |realm| self.to_js(&CallContext::new(vm, realm)))
        }
"""
        )
    out.write("    }\n")
