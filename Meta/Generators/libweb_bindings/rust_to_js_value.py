# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

# The Rust expressions that convert IDL values to JS values, https://webidl.spec.whatwg.org/#js-type-mapping, for the
# Rust bindings of the interfaces implemented in Rust, as to_js_value.py writes the C++ ones. The bindings runtime
# implements ToJs for the types whose conversion does not depend on the IDL type, and this module writes the
# implementations for the definitions of the IDL.

from typing import TextIO

from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.rust_names import rust_identifier
from Generators.libweb_bindings.rust_names import rust_local_name
from Generators.libweb_bindings.rust_types import UnsupportedByRustBindings
from Generators.libweb_bindings.rust_types import holds_cells
from Generators.libweb_bindings.rust_types import member_other_than_undefined
from Generators.libweb_bindings.rust_types import rust_dictionary_path
from Generators.libweb_bindings.rust_types import rust_enumeration_path
from Generators.libweb_bindings.rust_types import rust_enumeration_variant
from Generators.libweb_bindings.rust_types import rust_type
from Generators.libweb_bindings.rust_types import rust_union_path
from Generators.libweb_bindings.rust_types import rust_union_variant
from Utils.webidl_parser import Dictionary
from Utils.webidl_parser import Enumeration
from Utils.webidl_parser import IDLParameterizedType
from Utils.webidl_parser import IDLType
from Utils.webidl_parser import IDLUnionType


def rust_to_js_value(
    idl_type: IDLType, expression: str, call_context: str, context: GenerationContext, what: str
) -> str:
    """An expression of type ThrowCompletionOr<Value> that converts the IDL value to a JS value. The call context is an
    expression of type &CallContext. Nullable types and unions with undefined both have an Option as their Rust type,
    so the conversion follows from the IDL type and not from the Rust one."""
    rust_type(idl_type, context, what, is_result=True)

    def inner(inner_type: IDLType) -> str:
        return rust_to_js_value(inner_type, "value", call_context, context, what)

    # https://webidl.spec.whatwg.org/#js-nullable-type
    if idl_type.nullable:
        return f"""match {expression} {{
            Some(value) => {inner(idl_type.without_nullable())},
            None => Ok(Value::NULL),
        }}"""
    optional_member = member_other_than_undefined(idl_type)
    if optional_member is not None:
        return f"""match {expression} {{
            Some(value) => {inner(optional_member)},
            None => Ok(Value::UNDEFINED),
        }}"""
    if isinstance(idl_type, IDLParameterizedType) and idl_type.name == "sequence":
        element_type = idl_type.parameters[0]
        # NB: The conversion of an element can allocate, as that of an object of an interface does for its wrapper,
        #     and the elements after it must live through that, in the MarkedVec that holds them (see rust_type()).
        function = "rooted_sequence_to_js" if holds_cells(element_type, context) else "sequence_to_js"
        return f"{function}({call_context}, {expression}, |value| {inner(element_type)})"
    if isinstance(idl_type, IDLParameterizedType) and idl_type.name == "record":
        value_type = idl_type.parameters[1]
        if holds_cells(value_type, context):
            # NB: The entries are in a Vec, which hides the values from the collector while those before them are
            #     converted.
            raise UnsupportedByRustBindings(
                f"{what} has the type {idl_type}, a record of values that are or can hold cells, which the Rust "
                "bindings do not convert yet"
            )
        return f"record_to_js({call_context}, {expression}, |value| {inner(value_type)})"
    return f"{expression}.to_js({call_context})"


def write_enumeration_to_js(out: TextIO, context: GenerationContext, enumeration: Enumeration) -> None:
    """The conversion of the enum of an enumeration, which matches every variant, so that an enum that does not match
    the values of the enumeration does not compile."""
    arms = "".join(
        f'                Self::{rust_enumeration_variant(value, enumeration)} => "{value}",\n'
        for value in enumeration.values
    )
    out.write(
        f"""
    /// The conversion of the enumeration {enumeration.name}, https://webidl.spec.whatwg.org/#js-enumeration
    impl ToJs for {rust_enumeration_path(enumeration, context)} {{
        fn to_js(self, cx: &CallContext) -> ThrowCompletionOr<Value> {{
            let enumeration_value = match self {{
{arms}            }};
            Ok(enumeration_value_to_js(cx, enumeration_value))
        }}
    }}
"""
    )


def write_union_to_js(out: TextIO, context: GenerationContext, union_type: IDLUnionType) -> None:
    """The conversion of the enum of a union, which the module of its typedef defines with a variant for each member,
    named as the type of the member and holding its IDL value."""
    what = f"the union {union_type}"
    path = rust_union_path(union_type, context, what)
    arms = "".join(
        f"""                Self::{rust_union_variant(member_type)}(value) => {rust_to_js_value(member_type, "value", "cx", context, what)},
"""
        for member_type in union_type.flattened_member_types()
    )
    out.write(
        f"""
    /// The conversion of the union {union_type}, https://webidl.spec.whatwg.org/#js-union
    impl ToJs for {path} {{
        fn to_js(self, cx: &CallContext) -> ThrowCompletionOr<Value> {{
            match self {{
{arms}            }}
        }}
    }}
"""
    )


def has_sequence_of_cells(idl_type: IDLType, context: GenerationContext) -> bool:
    """Whether the IDL values of the type have a sequence of values that are or can hold cells, however deep in the
    type the sequence is."""
    return any(
        isinstance(nested_type, IDLParameterizedType)
        and nested_type.name == "sequence"
        and holds_cells(nested_type, context)
        for nested_type in idl_type.nested_types()
    )


def write_dictionary_to_js(out: TextIO, context: GenerationContext, dictionary: Dictionary) -> None:
    """The conversion of the dictionary's struct, which the implementation defines with a field per member: the type
    of a required member or of one with a default value, and an Option of it otherwise. The fields are bound to locals
    with names of their own, so that a member can have any name, and the keys of the members are interned once."""
    path = rust_dictionary_path(dictionary, context)
    fields = []
    keys = []
    conversions = []
    for member in dictionary.members:
        what = f"{dictionary.name}.{member.name}"
        member_type = rust_type(member.type, context, what)
        if has_sequence_of_cells(member.type, context):
            # NB: A sequence is kept alive from where its conversion starts, which is after the object of the
            #     dictionary is created and the members before this one are converted.
            raise UnsupportedByRustBindings(
                f"{what} has the type {member.type}, with a sequence of values that are or can hold cells, which the "
                "Rust bindings do not convert from a member of a dictionary yet"
            )
        field = rust_identifier(member.name)
        local = rust_local_name(member.name)
        key = f"{rust_identifier(member.name).removeprefix('r#').rstrip('_').upper()}_KEY"
        keys.append(
            f'            static {key}: LazyLock<PropertyKey> = LazyLock::new(|| PropertyKey::from_utf8("{member.name}"));\n'
        )
        always_present = member.required or member.default_value is not None
        if always_present:
            conversion = rust_to_js_value(member.type, local, "cx", context, what)
            fields.append(f"    //   {field}: {member_type},\n")
            conversions.append(f"            set_dictionary_member(cx, object, &{key}, {conversion}?)?;\n")
        else:
            conversion = rust_to_js_value(member.type, "value", "cx", context, what)
            fields.append(f"    //   {field}: Option<{member_type}>,\n")
            conversions.append(
                f"""            if let Some(value) = {local} {{
                set_dictionary_member(cx, object, &{key}, {conversion}?)?;
            }}
"""
            )
    bindings = ", ".join(
        f"{rust_identifier(member.name)}: {rust_local_name(member.name)}" for member in dictionary.members
    )
    out.write(
        f"""
    /// The conversion of the dictionary {dictionary.name}, a struct with the fields:
{"".join(fields)}    impl ToJs for {path} {{
        fn to_js(self, cx: &CallContext) -> ThrowCompletionOr<Value> {{
{"".join(keys)}            let {path} {{ {bindings} }} = self;
            let object = create_dictionary_object(cx);
{"".join(conversions)}            Ok(Value::from_object(object))
        }}
    }}
"""
    )
