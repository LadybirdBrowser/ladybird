# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from dataclasses import replace
from typing import Callable
from typing import Optional
from typing import Union

from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.cpp_types import CppType
from Generators.libweb_bindings.cpp_types import cpp_null_value
from Generators.libweb_bindings.cpp_types import cpp_type
from Generators.libweb_bindings.cpp_types import cpp_type_for_idl_type
from Generators.libweb_bindings.cpp_types import cpp_type_for_idl_type_details
from Generators.libweb_bindings.cpp_types import is_numeric_type
from Generators.libweb_bindings.cpp_types import is_string_type
from Utils.utils import string_to_cpp_enum_name
from Utils.webidl_parser import DictionaryMember
from Utils.webidl_parser import IDLParameterizedType
from Utils.webidl_parser import IDLType
from Utils.webidl_parser import IDLUnionType
from Utils.webidl_parser import OperationParameter

DefaultValueMember = Union[DictionaryMember, OperationParameter]


def string_default_value_expression(cpp_type: CppType, default_value: str) -> str:
    if cpp_type.name == "String":
        return "String {}" if default_value == '""' else f"{default_value}_string"
    if cpp_type.name == "FlyString":
        return "FlyString {}" if default_value == '""' else f"{default_value}_fly_string"
    if cpp_type.name == "ByteString":
        return "ByteString {}" if default_value == '""' else f"{default_value}sv"
    if cpp_type.name == "Utf16String":
        return "Utf16String {}" if default_value == '""' else f"{default_value}_utf16"
    if cpp_type.name == "Utf16FlyString":
        return "Utf16FlyString {}" if default_value == '""' else f"{default_value}_utf16_fly_string"
    raise RuntimeError(f"Unsupported string default value type '{cpp_type.name}'")


def first_flattened_member_type_matching(
    union_type: IDLUnionType,
    predicate: Callable[[IDLType], bool],
) -> Optional[IDLType]:
    return next((member_type for member_type in union_type.flattened_member_types() if predicate(member_type)), None)


def is_numeric_default_value(default_value: str) -> bool:
    try:
        int(default_value, 0)
        return True
    except ValueError:
        pass

    try:
        float(default_value)
        return True
    except ValueError:
        return False


def union_member_type_for_default_value(
    union_type: IDLUnionType,
    default_value: str,
    context: GenerationContext,
) -> IDLType:
    # Default values are stored as raw IDL text. For unions, pick the union member type that can represent
    # that literal so cpp_default_value_conversion() can emit the typed C++ expression for that member.
    if default_value == "[]":
        sequence_type = first_flattened_member_type_matching(
            union_type,
            lambda member_type: (
                isinstance(member_type, IDLParameterizedType) and member_type.name in ("sequence", "FrozenArray")
            ),
        )
        if sequence_type is not None:
            return sequence_type

    if default_value == "{}":

        def accepts_empty_object(member_type: IDLType) -> bool:
            return (
                isinstance(member_type, IDLParameterizedType)
                and member_type.name == "record"
                or context.dictionary(member_type) is not None
            )

        object_type = first_flattened_member_type_matching(
            union_type,
            accepts_empty_object,
        )
        if object_type is not None:
            return object_type

    if default_value.startswith('"') and default_value.endswith('"'):
        string_type = first_flattened_member_type_matching(
            union_type,
            lambda member_type: is_string_type(member_type.name),
        )
        if string_type is not None:
            return string_type

        enum_value = default_value.removeprefix('"').removesuffix('"')

        def accepts_enum_value(member_type: IDLType) -> bool:
            enumeration = context.enumeration(member_type)
            return enumeration is not None and enum_value in enumeration.values

        enum_type = first_flattened_member_type_matching(
            union_type,
            accepts_enum_value,
        )
        if enum_type is not None:
            return enum_type

    if default_value in ("true", "false"):
        boolean_type = first_flattened_member_type_matching(
            union_type,
            lambda member_type: member_type.name == "boolean",
        )
        if boolean_type is not None:
            return boolean_type

    if is_numeric_default_value(default_value):
        numeric_type = first_flattened_member_type_matching(
            union_type,
            lambda member_type: is_numeric_type(member_type.name),
        )
        if numeric_type is not None:
            return numeric_type

    if default_value == "null" and (union_type.includes_undefined() or union_type.includes_nullable_type()):
        return IDLType("undefined")

    raise RuntimeError(f"Unsupported union default value '{default_value}' for '{union_type}'")


def cpp_default_value_conversion(
    member: DefaultValueMember,
    context: GenerationContext,
) -> str:
    if member.default_value is None:
        member_kind = "operation parameter" if isinstance(member, OperationParameter) else "dictionary member"
        raise RuntimeError(f"{member_kind.capitalize()} '{member.name}' has no default value")

    member_type = member.type
    if isinstance(member_type, IDLUnionType):
        union_member_type = union_member_type_for_default_value(member_type, member.default_value, context)
        if union_member_type.name == "undefined":
            return f"{cpp_type_for_idl_type(member_type, context)} {{ Empty {{}} }}"

        union_member = replace(member, type=union_member_type)
        expression = cpp_default_value_conversion(union_member, context)
        return f"{cpp_type_for_idl_type(member_type, context)} {{ {expression} }}"

    if member.default_value == "{}":
        return f"{member_type.name} {{}}"
    if member.default_value == "null":
        if member_type.name == "any":
            return "JS::js_null()"
        return cpp_null_value(member_type, context)
    if member_type.name == "boolean":
        return member.default_value
    if is_numeric_type(member_type.name):
        return member.default_value
    if (
        member.default_value == "[]"
        and isinstance(member_type, IDLParameterizedType)
        and member_type.name in ("sequence", "FrozenArray")
    ):
        return f"{cpp_type(member, context)} {{}}"
    if member.default_value.startswith('"') and member.default_value.endswith('"'):
        if (enumeration := context.enumeration(member_type)) is not None:
            unquoted_default_value = member.default_value.removeprefix('"').removesuffix('"')
            return f"{enumeration.name}::{string_to_cpp_enum_name(unquoted_default_value)}"
        string_cpp_type = cpp_type_for_idl_type_details(
            member_type.without_nullable(),
            context,
            extended_attributes=member.extended_attributes,
        )
        return string_default_value_expression(string_cpp_type, member.default_value)
    raise RuntimeError(f"Unsupported default value for dictionary member '{member.name}'")
