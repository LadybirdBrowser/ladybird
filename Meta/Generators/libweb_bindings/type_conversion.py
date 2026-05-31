# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from Generators.libweb_bindings.includes import GeneratedIncludes
from Utils.utils import make_name_acceptable_cpp
from Utils.utils import title_casify
from Utils.utils import title_case_to_snake_case
from Utils.webidl_parser import DictionaryMember


def cpp_name(member: DictionaryMember) -> str:
    return make_name_acceptable_cpp(title_case_to_snake_case(member.name))


def cpp_type(member: DictionaryMember) -> str:
    if member.type == "boolean":
        return "bool"
    if member.type == "unrestricted double":
        return "double"
    return member.type


def add_header_includes_for_type(member: DictionaryMember, includes: GeneratedIncludes, local_types: set[str]) -> None:
    if cpp_type(member) == member.type and member.type not in local_types:
        includes.add_binding(member.type)


def boolean_to_idl_value(value_name: str, includes: GeneratedIncludes) -> str:
    includes.add("LibJS/Runtime/Value.h")

    # https://webidl.spec.whatwg.org/#js-boolean
    # 1. Let x be the result of computing ToBoolean(V).
    # 2. Return the IDL boolean value that is the one that represents the same truth value as the JavaScript Boolean value x.
    return f"{value_name}.to_boolean()"


def unrestricted_double_to_idl_value(value_name: str, includes: GeneratedIncludes) -> str:
    includes.add("LibJS/Runtime/ValueInlines.h")

    # https://webidl.spec.whatwg.org/#js-unrestricted-float
    return f"{value_name}.to_double(vm)"


def to_idl_value(member: DictionaryMember, value_name: str, includes: GeneratedIncludes) -> str:
    if member.type == "boolean":
        return boolean_to_idl_value(value_name, includes)
    if member.type == "unrestricted double":
        return unrestricted_double_to_idl_value(value_name, includes)
    return f"convert_to_idl_value_for_{make_name_acceptable_cpp(title_case_to_snake_case(member.type))}(vm, {value_name})"


def cpp_default_value_conversion(member: DictionaryMember) -> str:
    if member.default_value is None:
        raise RuntimeError(f"Dictionary member '{member.name}' has no default value")
    if member.type == "boolean":
        if member.default_value == "true":
            return "true"
        if member.default_value == "false":
            return "false"
    if member.default_value.startswith('"') and member.default_value.endswith('"'):
        return f"{cpp_type(member)}::{title_casify(member.default_value.removeprefix('\"').removesuffix('\"'))}"
    raise RuntimeError(f"Unsupported default value for dictionary member '{member.name}'")
