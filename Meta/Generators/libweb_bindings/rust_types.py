# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

# The Rust types of IDL values, and the Rust expressions that convert JS values to them, for the Rust bindings of the
# interfaces implemented in Rust. They cover the types that those interfaces use so far; every other type is an error.

from typing import Optional

from Generators.libweb_bindings.context import GenerationContext
from Utils.webidl_parser import Dictionary
from Utils.webidl_parser import IDLType
from Utils.webidl_parser import OperationParameter

# The typed arrays, whose names are the variants of the runtime's TypedArrayKind.
TYPED_ARRAY_NAMES = {
    "Int8Array",
    "Uint8Array",
    "Uint8ClampedArray",
    "Int16Array",
    "Uint16Array",
    "Int32Array",
    "Uint32Array",
    "BigInt64Array",
    "BigUint64Array",
    "Float16Array",
    "Float32Array",
    "Float64Array",
}

# The extended attributes of a type or a parameter that the Rust bindings handle.
SUPPORTED_TYPE_EXTENDED_ATTRIBUTES = {"AllowShared", "AllowResizable"}


class UnsupportedByRustBindings(Exception):
    pass


def resolved_dictionary(idl_type: IDLType, context: GenerationContext) -> Optional[Dictionary]:
    return context.dictionaries.get(idl_type.name)


def check_type_is_supported(idl_type: IDLType, context: GenerationContext, what: str) -> None:
    unsupported_attributes = set(idl_type.extended_attributes) - SUPPORTED_TYPE_EXTENDED_ATTRIBUTES
    if unsupported_attributes:
        raise UnsupportedByRustBindings(f"{what} has extended attributes {sorted(unsupported_attributes)}")
    if idl_type.nullable:
        raise UnsupportedByRustBindings(f"{what} has the nullable type {idl_type}")
    rust_type(idl_type, context, what)


def rust_type(idl_type: IDLType, context: GenerationContext, what: str) -> str:
    """The Rust type of the IDL values of the type."""
    name = idl_type.name
    if name == "DOMString":
        return "Utf16String"
    if name == "USVString":
        return "UsvString"
    if name == "boolean":
        return "bool"
    if name == "unsigned long long":
        return "u64"
    if name == "undefined":
        return "()"
    if name in TYPED_ARRAY_NAMES:
        return "Gc<TypedArrayBase>"
    if resolved_dictionary(idl_type, context) is not None:
        return name
    raise UnsupportedByRustBindings(f"{what} has the type {idl_type}, which the Rust bindings do not convert yet")


def rust_parameter_type(parameter: OperationParameter, context: GenerationContext, what: str) -> str:
    """The Rust type the implementation receives for the parameter."""
    value_type = rust_type(parameter.type, context, what)
    if parameter.optional and parameter.default_value is None:
        return f"Option<{value_type}>"
    return value_type


def rust_conversion_to_idl_value(
    idl_type: IDLType, parameter_attributes: dict[str, str], value: str, context: GenerationContext, what: str
) -> str:
    """An expression of type ThrowCompletionOr<T> that converts the JS value to the IDL value."""
    name = idl_type.name
    if name == "DOMString":
        return f"to_dom_string(vm, {value})"
    if name == "USVString":
        return f"to_usv_string(vm, {value})"
    if name in TYPED_ARRAY_NAMES:
        attributes = {**idl_type.extended_attributes, **parameter_attributes}
        allow_shared = "true" if "AllowShared" in attributes else "false"
        allow_resizable = "true" if "AllowResizable" in attributes else "false"
        return (
            f'to_typed_array(vm, {value}, TypedArrayKind::{name}, "{name}", '
            f"&BufferSourceRules {{ allow_shared: {allow_shared}, allow_resizable: {allow_resizable} }})"
        )
    raise UnsupportedByRustBindings(f"{what} has the type {idl_type}, which the Rust bindings do not convert to yet")


def rust_default_value(parameter: OperationParameter, context: GenerationContext, what: str) -> str:
    """An expression of the parameter's default value."""
    name = parameter.type.name
    default_value = parameter.default_value
    if name in ("DOMString", "USVString") and default_value is not None and default_value.startswith('"'):
        string = default_value[1:-1]
        if not string.isascii() or "\\" in string:
            raise UnsupportedByRustBindings(f"{what} has the default value {default_value}")
        if name == "USVString":
            return f'UsvString::from_ascii(b"{string}")'
        return f'Utf16String::from_ascii(b"{string}")'
    raise UnsupportedByRustBindings(
        f"{what} has the default value {default_value}, which the Rust bindings do not have"
    )
