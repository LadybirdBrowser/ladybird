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
from Generators.libweb_bindings.rust_types import rust_dictionary_path
from Generators.libweb_bindings.rust_types import rust_type
from Utils.webidl_parser import Dictionary
from Utils.webidl_parser import IDLType


def rust_to_js_value(
    idl_type: IDLType, expression: str, call_context: str, context: GenerationContext, what: str
) -> str:
    """An expression of type ThrowCompletionOr<Value> that converts the IDL value to a JS value. The call context is an
    expression of type &CallContext."""
    rust_type(idl_type, context, what)
    return f"{expression}.to_js({call_context})"


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
