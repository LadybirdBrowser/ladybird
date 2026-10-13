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
from Utils.webidl_parser import IDLParameterizedType
from Utils.webidl_parser import IDLType
from Utils.webidl_parser import IDLUnionType
from Utils.webidl_parser import Interface
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

# The integer types, with the Rust type of their values.
INTEGER_TYPES = {"long": "i32", "unsigned long": "u32", "long long": "i64", "unsigned long long": "u64"}

# The floating point types, with the Rust type of their values.
FLOATING_POINT_TYPES = {"float": "f32", "unrestricted float": "f32", "unrestricted double": "f64"}

# The unions that the bindings runtime has a type for, by the name of the typedef that names the union.
RUNTIME_UNION_TYPES = {"BufferSource": "BufferSource"}

DECIMAL_LITERAL = re.compile(r"-?[0-9]+(\.[0-9]+)?([eE][-+]?[0-9]+)?")

# https://webidl.spec.whatwg.org/#prod-integer
INTEGER_LITERAL = re.compile(r"-?([1-9][0-9]*|0[Xx][0-9A-Fa-f]+|0[0-7]*)")


class UnsupportedByRustBindings(Exception):
    pass


def rust_integer_literal(literal: Optional[str], idl_type_name: str) -> Optional[str]:
    """The Rust literal of the value of an integer token of the IDL, or None if the token is not one or its value is
    not one of the integer type."""
    if literal is None or not INTEGER_LITERAL.fullmatch(literal):
        return None
    digits = literal.removeprefix("-")
    if digits[:2] in ("0x", "0X"):
        magnitude = int(digits, 16)
    elif digits.startswith("0"):
        magnitude = int(digits, 8)
    else:
        magnitude = int(digits, 10)
    integer = -magnitude if literal.startswith("-") else magnitude
    rust_integer_type = INTEGER_TYPES[idl_type_name]
    bits = int(rust_integer_type[1:])
    if rust_integer_type.startswith("u"):
        lowest, highest = 0, 2**bits - 1
    else:
        lowest, highest = -(2 ** (bits - 1)), 2 ** (bits - 1) - 1
    if not lowest <= integer <= highest:
        return None
    return f"{integer}_{rust_integer_type}"


def is_implemented_in_rust(interface: Optional[Interface]) -> bool:
    return interface is not None and interface.extended_attributes.get("ImplementedIn") == "Rust"


def resolved_dictionary(idl_type: IDLType, context: GenerationContext) -> Optional[Dictionary]:
    return context.dictionaries.get(idl_type.name)


def rust_interface(idl_type: IDLType, context: GenerationContext) -> Optional[Interface]:
    """The interface that the type names, if it is implemented in Rust."""
    if isinstance(idl_type, (IDLParameterizedType, IDLUnionType)):
        return None
    interface = context.interfaces.get(idl_type.name)
    return interface if is_implemented_in_rust(interface) else None


def rust_module_path(idl_path: Path) -> str:
    """The Rust module of the hand-written definitions of an IDL file, which follows from its path:
    Encoding/TextEncoder.idl names crate::encoding::text_encoder."""
    parts = idl_path.parts
    namespace = parts[parts.index("LibWeb") + 1]
    return f"crate::{title_case_to_snake_case(namespace)}::{title_case_to_snake_case(idl_path.stem)}"


def rust_interface_path(interface: Interface) -> str:
    """The Rust path of the implementation of an interface: crate::encoding::text_encoder::TextEncoder for
    Encoding/TextEncoder.idl."""
    return f"{rust_module_path(interface.path)}::{interface.name}"


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


def member_other_than_undefined(idl_type: IDLType) -> Optional[IDLType]:
    """The other member of a union of a type and undefined, whose IDL values are those of the type or nothing."""
    if not isinstance(idl_type, IDLUnionType) or idl_type.nullable or len(idl_type.member_types) != 2:
        return None
    others = [member for member in idl_type.member_types if member.name != "undefined"]
    return others[0] if len(others) == 1 else None


def is_named_union(idl_type: IDLType, context: GenerationContext) -> bool:
    """Whether the Rust type of the union is an enum that the module of the typedef of the union defines."""
    return (
        isinstance(idl_type, IDLUnionType)
        and runtime_union_type(idl_type, context) is None
        and member_other_than_undefined(idl_type) is None
    )


def rust_union_variant(member_type: IDLType) -> str:
    """The variant of a member of a union in the enum of the union, which has the name of the type of the member."""
    return member_type.name.replace(" ", "")


def rust_union_path(idl_type: IDLUnionType, context: GenerationContext, what: str) -> str:
    """The Rust path of the enum of a union, which is in the module of the IDL file whose typedef names the union:
    crate::url_pattern::url_pattern::URLPatternInput."""
    name = union_typedef_name(idl_type, context, what)
    for module in context.modules:
        if any(typedef.name == name for typedef in module.typedefs):
            return f"{rust_module_path(module.path)}::{name}"
    raise RuntimeError(f"No module defines the typedef {name}")


def rust_type(idl_type: IDLType, context: GenerationContext, what: str) -> str:
    """The Rust type of the IDL values of the type."""
    unsupported_attributes = set(idl_type.extended_attributes) - SUPPORTED_TYPE_EXTENDED_ATTRIBUTES
    if unsupported_attributes:
        raise UnsupportedByRustBindings(f"{what} has extended attributes {sorted(unsupported_attributes)}")
    if idl_type.nullable:
        return f"Option<{rust_type(idl_type.without_nullable(), context, what)}>"
    name = idl_type.name
    if isinstance(idl_type, IDLParameterizedType):
        if name == "sequence":
            return f"Vec<{rust_type(idl_type.parameters[0], context, what)}>"
        if name == "record":
            key_type, value_type = idl_type.parameters
            if key_type.name not in ("DOMString", "USVString"):
                raise UnsupportedByRustBindings(f"{what} has a record with keys of the type {key_type}")
            return f"Vec<({rust_type(key_type, context, what)}, {rust_type(value_type, context, what)})>"
        raise UnsupportedByRustBindings(f"{what} has the type {idl_type}, which the Rust bindings do not convert yet")
    if name == "DOMString":
        return "Utf16String"
    if name == "USVString":
        return "UsvString"
    if name == "boolean":
        return "bool"
    if name in INTEGER_TYPES:
        return INTEGER_TYPES[name]
    if name in FLOATING_POINT_TYPES:
        return FLOATING_POINT_TYPES[name]
    if name == "undefined":
        return "()"
    if name == "any":
        return "Value"
    if name == "object":
        return "Gc<Object>"
    if name in TYPED_ARRAY_NAMES:
        return "Gc<TypedArrayBase>"
    if context.callback_function(idl_type) is not None:
        return "Gc<FunctionObject>"
    interface = rust_interface(idl_type, context)
    if interface is not None:
        return f"Ref<{rust_interface_path(interface)}>"
    dictionary = resolved_dictionary(idl_type, context)
    if dictionary is not None:
        return rust_dictionary_path(dictionary, context)
    runtime_type = runtime_union_type(idl_type, context)
    if runtime_type is not None:
        return runtime_type
    optional_member = member_other_than_undefined(idl_type)
    if optional_member is not None:
        return f"Option<{rust_type(optional_member, context, what)}>"
    if isinstance(idl_type, IDLUnionType):
        for member_type in idl_type.flattened_member_types():
            rust_type(member_type, context, what)
        return rust_union_path(idl_type, context, what)
    raise UnsupportedByRustBindings(f"{what} has the type {idl_type}, which the Rust bindings do not convert yet")


def is_promise_type(idl_type: IDLType) -> bool:
    return isinstance(idl_type, IDLParameterizedType) and idl_type.name == "Promise"


def rust_result_type(idl_type: IDLType, context: GenerationContext, what: str) -> str:
    """The Rust type that the method of an operation returns. An operation that returns a promise type returns the
    promise object, which is the only place the Rust bindings handle a promise type in."""
    if is_promise_type(idl_type) and not idl_type.nullable:
        return "Gc<Object>"
    return rust_type(idl_type, context, what)


def holds_cells(idl_type: IDLType, context: GenerationContext) -> bool:
    """Whether the Rust type of the IDL values of the type is or can hold a cell, which a Vec would hide from the
    collector."""
    idl_type = idl_type.without_nullable()
    if isinstance(idl_type, (IDLParameterizedType, IDLUnionType)):
        return False
    if idl_type.name in ("any", "object") or idl_type.name in TYPED_ARRAY_NAMES:
        return True
    return context.callback_function(idl_type) is not None or rust_interface(idl_type, context) is not None


def rust_parameter_type(parameter: OperationParameter, context: GenerationContext, what: str) -> str:
    """The Rust type the implementation receives for the parameter."""
    value_type = rust_type(parameter.type, context, what)
    if parameter.optional and parameter.default_value is None:
        return f"Option<{value_type}>"
    return value_type
