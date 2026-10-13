# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

# The Rust types of IDL values, for the Rust bindings of the interfaces implemented in Rust. They cover the types that
# those interfaces use so far; every other type is an error.

import re

from pathlib import Path
from typing import Optional

from Generators.libweb_bindings.context import GenerationContext
from Utils.utils import title_case_to_snake_case
from Utils.webidl_parser import Dictionary
from Utils.webidl_parser import IDLType
from Utils.webidl_parser import IDLUnionType
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

# The floating point types, with the Rust type of their values.
FLOATING_POINT_TYPES = {"float": "f32"}

# The unions that the bindings runtime has a type for, by the name of the typedef that names the union.
RUNTIME_UNION_TYPES = {"BufferSource": "BufferSource"}

DECIMAL_LITERAL = re.compile(r"-?[0-9]+(\.[0-9]+)?([eE][-+]?[0-9]+)?")


class UnsupportedByRustBindings(Exception):
    pass


def resolved_dictionary(idl_type: IDLType, context: GenerationContext) -> Optional[Dictionary]:
    return context.dictionaries.get(idl_type.name)


def rust_module_path(idl_path: Path) -> str:
    """The Rust module of the hand-written definitions of an IDL file, which follows from its path:
    Encoding/TextEncoder.idl names crate::encoding::text_encoder."""
    parts = idl_path.parts
    namespace = parts[parts.index("LibWeb") + 1]
    return f"crate::{title_case_to_snake_case(namespace)}::{title_case_to_snake_case(idl_path.stem)}"


def rust_dictionary_path(dictionary: Dictionary, context: GenerationContext) -> str:
    """The Rust path of the struct of a dictionary, which is in the module of the IDL file that defines the
    dictionary: crate::encoding::text_encoder::TextEncoderEncodeIntoResult."""
    for module in context.modules:
        if any(defined.name == dictionary.name for defined in module.dictionaries):
            return f"{rust_module_path(module.path)}::{dictionary.name}"
    raise RuntimeError(f"No module defines the dictionary {dictionary.name}")


def union_typedef_name(idl_type: IDLUnionType, context: GenerationContext, what: str) -> str:
    """The name of the typedef that names the union. The generator resolves typedefs before it converts types, and the
    Rust type of a union needs a name, so the typedef is found again by its type."""
    names = sorted(
        name
        for name, typedef in context.typedefs.items()
        if typedef.type.without_nullable() == idl_type.without_nullable()
    )
    if not names:
        raise UnsupportedByRustBindings(f"{what} has the union type {idl_type}, which no typedef names")
    if len(names) > 1:
        raise UnsupportedByRustBindings(f"{what} has the union type {idl_type}, which the typedefs {names} all name")
    return names[0]


def runtime_union_type(idl_type: IDLType, context: GenerationContext) -> Optional[str]:
    """The type of the bindings runtime for the union, if it has one."""
    if not isinstance(idl_type, IDLUnionType):
        return None
    for name, runtime_type in RUNTIME_UNION_TYPES.items():
        typedef = context.typedefs.get(name)
        if typedef is not None and typedef.type.without_nullable() == idl_type.without_nullable():
            return runtime_type
    return None


def rust_type(idl_type: IDLType, context: GenerationContext, what: str) -> str:
    """The Rust type of the IDL values of the type."""
    unsupported_attributes = set(idl_type.extended_attributes) - SUPPORTED_TYPE_EXTENDED_ATTRIBUTES
    if unsupported_attributes:
        raise UnsupportedByRustBindings(f"{what} has extended attributes {sorted(unsupported_attributes)}")
    if idl_type.nullable:
        raise UnsupportedByRustBindings(f"{what} has the nullable type {idl_type}")
    name = idl_type.name
    if name == "DOMString":
        return "Utf16String"
    if name == "USVString":
        return "UsvString"
    if name == "boolean":
        return "bool"
    if name == "unsigned long long":
        return "u64"
    if name in FLOATING_POINT_TYPES:
        return FLOATING_POINT_TYPES[name]
    if name == "undefined":
        return "()"
    if name in TYPED_ARRAY_NAMES:
        return "Gc<TypedArrayBase>"
    dictionary = resolved_dictionary(idl_type, context)
    if dictionary is not None:
        return rust_dictionary_path(dictionary, context)
    runtime_type = runtime_union_type(idl_type, context)
    if runtime_type is not None:
        return runtime_type
    if isinstance(idl_type, IDLUnionType):
        union_typedef_name(idl_type, context, what)
    raise UnsupportedByRustBindings(f"{what} has the type {idl_type}, which the Rust bindings do not convert yet")


def rust_parameter_type(parameter: OperationParameter, context: GenerationContext, what: str) -> str:
    """The Rust type the implementation receives for the parameter."""
    value_type = rust_type(parameter.type, context, what)
    if parameter.optional and parameter.default_value is None:
        return f"Option<{value_type}>"
    return value_type
