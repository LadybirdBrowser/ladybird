# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

# The Rust expressions that convert JS values to IDL values, https://webidl.spec.whatwg.org/#js-type-mapping, for the
# Rust bindings of the interfaces implemented in Rust, as to_idl_value.py writes the C++ ones. The conversions run in
# the order of the C++ bindings and throw their errors, so that an interface behaves the same in either language.

from typing import TextIO
from typing import Union

from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.rust_names import rust_identifier
from Generators.libweb_bindings.rust_names import rust_local_name
from Generators.libweb_bindings.rust_types import DECIMAL_LITERAL
from Generators.libweb_bindings.rust_types import FLOATING_POINT_TYPES
from Generators.libweb_bindings.rust_types import INTEGER_TYPES
from Generators.libweb_bindings.rust_types import TYPED_ARRAY_NAMES
from Generators.libweb_bindings.rust_types import UnsupportedByRustBindings
from Generators.libweb_bindings.rust_types import is_named_union
from Generators.libweb_bindings.rust_types import member_other_than_undefined
from Generators.libweb_bindings.rust_types import resolved_dictionary
from Generators.libweb_bindings.rust_types import runtime_union_type
from Generators.libweb_bindings.rust_types import rust_dictionary_path
from Generators.libweb_bindings.rust_types import rust_integer_literal
from Generators.libweb_bindings.rust_types import rust_interface
from Generators.libweb_bindings.rust_types import rust_interface_path
from Generators.libweb_bindings.rust_types import rust_type
from Generators.libweb_bindings.rust_types import rust_union_path
from Generators.libweb_bindings.rust_types import rust_union_variant
from Utils.webidl_parser import Dictionary
from Utils.webidl_parser import DictionaryMember
from Utils.webidl_parser import IDLParameterizedType
from Utils.webidl_parser import IDLType
from Utils.webidl_parser import IDLUnionType
from Utils.webidl_parser import OperationParameter

STRING_TYPES = ("DOMString", "USVString")


def buffer_source_rules(idl_type: IDLType, parameter_attributes: dict[str, str]) -> str:
    attributes = {**idl_type.extended_attributes, **parameter_attributes}
    allow_shared = "true" if "AllowShared" in attributes else "false"
    allow_resizable = "true" if "AllowResizable" in attributes else "false"
    return f"&BufferSourceRules {{ allow_shared: {allow_shared}, allow_resizable: {allow_resizable} }}"


def rust_to_idl_value(
    idl_type: IDLType,
    parameter_attributes: dict[str, str],
    value: str,
    identifier: str,
    context: GenerationContext,
    what: str,
) -> str:
    """An expression of type ThrowCompletionOr<T> that converts the JS value to the IDL value. The identifier is the
    name of the parameter or member that the value is of, which the errors of some conversions name."""
    rust_type(idl_type, context, what)
    name = idl_type.name
    # https://webidl.spec.whatwg.org/#js-nullable-type
    if idl_type.nullable:
        inner_type = idl_type.without_nullable()
        if member_other_than_undefined(inner_type) is not None:
            raise UnsupportedByRustBindings(
                f"{what} has the type {idl_type}, whose inner type includes undefined, which the Rust bindings do "
                "not convert to yet"
            )
        inner = rust_to_idl_value(inner_type, parameter_attributes, value, identifier, context, what)
        return f"if {value}.is_nullish() {{ Ok(None) }} else {{ {inner}.map(Some) }}"
    if isinstance(idl_type, IDLParameterizedType):
        raise UnsupportedByRustBindings(
            f"{what} has the type {idl_type}, which the Rust bindings do not convert to yet"
        )
    if name == "DOMString":
        return f"to_dom_string(vm, {value})"
    if name == "USVString":
        return f"to_usv_string(vm, {value})"
    if name == "boolean":
        return f"to_boolean({value})"
    if name in INTEGER_TYPES:
        return f"to_{name.replace(' ', '_')}(vm, {value})"
    if name == "float":
        return f'to_float(vm, {value}, "{identifier}")'
    if name == "unrestricted float":
        return f"to_unrestricted_float(vm, {value})"
    if name == "unrestricted double":
        return f"to_unrestricted_double(vm, {value})"
    if name in TYPED_ARRAY_NAMES:
        rules = buffer_source_rules(idl_type, parameter_attributes)
        return f'to_typed_array(vm, {value}, TypedArrayKind::{name}, "{name}", {rules})'
    if runtime_union_type(idl_type, context) == "BufferSource":
        return f"to_buffer_source(vm, {value}, {buffer_source_rules(idl_type, parameter_attributes)})"
    interface = rust_interface(idl_type, context)
    if interface is not None:
        return f"to_interface::<{rust_interface_path(interface)}>(vm, {value})"
    dictionary = resolved_dictionary(idl_type, context)
    if dictionary is not None:
        return f"<{rust_dictionary_path(dictionary, context)}>::from_js(vm, {value})"
    if isinstance(idl_type, IDLUnionType) and is_named_union(idl_type, context):
        union_members_by_kind(idl_type, context, what)
        return f"<{rust_union_path(idl_type, context, what)}>::from_js(vm, {value})"
    raise UnsupportedByRustBindings(f"{what} has the type {idl_type}, which the Rust bindings do not convert to yet")


def union_members_by_kind(union_type: IDLUnionType, context: GenerationContext, what: str):
    """The dictionary type and the string type among the members of a union, which are the kinds of member that the
    Rust bindings distinguish between when they convert a JS value to a union."""
    dictionary_type = None
    string_type = None
    for member_type in union_type.flattened_member_types():
        if resolved_dictionary(member_type, context) is not None and dictionary_type is None:
            dictionary_type = member_type
        elif member_type.name in STRING_TYPES and string_type is None:
            string_type = member_type
        else:
            raise UnsupportedByRustBindings(
                f"{what} has the union type {union_type}, and the Rust bindings do not convert a JS value to a "
                f"union with the member {member_type} yet"
            )
    return dictionary_type, string_type


def rust_default_value(
    member: Union[OperationParameter, DictionaryMember], context: GenerationContext, what: str
) -> str:
    """An expression of the default value of the parameter or of the dictionary member."""
    name = member.type.name
    default_value = member.default_value
    if name in ("DOMString", "USVString") and default_value is not None and default_value.startswith('"'):
        string = default_value[1:-1]
        if not string.isascii() or "\\" in string:
            raise UnsupportedByRustBindings(f"{what} has the default value {default_value}")
        if name == "USVString":
            return f'UsvString::from_ascii(b"{string}")'
        return f'Utf16String::from_ascii(b"{string}")'
    if name == "boolean" and default_value in ("true", "false"):
        return default_value
    if name in FLOATING_POINT_TYPES and default_value is not None and DECIMAL_LITERAL.fullmatch(default_value):
        return f"{default_value}_{FLOATING_POINT_TYPES[name]}"
    if name in INTEGER_TYPES:
        integer_literal = rust_integer_literal(default_value, name)
        if integer_literal is not None:
            return integer_literal
    dictionary = resolved_dictionary(member.type, context)
    if dictionary is not None and default_value == "{}":
        # NB: The C++ bindings do not run the conversion for a missing argument either, so the members of the default
        #     value are not read from a JS object.
        return f"<{rust_dictionary_path(dictionary, context)}>::empty()"
    if isinstance(member.type, IDLUnionType) and is_named_union(member.type, context) and default_value == "{}":
        dictionary_type, _ = union_members_by_kind(member.type, context, what)
        if dictionary_type is not None:
            dictionary = context.dictionaries[dictionary_type.name]
            union_path = rust_union_path(member.type, context, what)
            return (
                f"{union_path}::{rust_union_variant(dictionary_type)}"
                f"(<{rust_dictionary_path(dictionary, context)}>::empty())"
            )
    raise UnsupportedByRustBindings(
        f"{what} has the default value {default_value}, which the Rust bindings do not have"
    )


def dictionary_can_be_empty(dictionary: Dictionary) -> bool:
    return not any(member.required for member in dictionary.members)


def write_dictionary_from_js(out: TextIO, context: GenerationContext, dictionary: Dictionary) -> None:
    """The conversion of a JS value to the dictionary's struct, which reads the members in the order of the C++
    bindings, the lexicographical one that the parser puts them in. The struct is built by naming every field, so that
    one that does not match the members of the dictionary does not compile."""
    path = rust_dictionary_path(dictionary, context)
    keys = []
    conversions = []
    empty_fields = []
    for member in dictionary.members:
        what = f"{dictionary.name}.{member.name}"
        local = rust_local_name(member.name)
        key = f"{rust_identifier(member.name).removeprefix('r#').rstrip('_').upper()}_KEY"
        keys.append(
            f'            static {key}: LazyLock<PropertyKey> = LazyLock::new(|| PropertyKey::from_utf8("{member.name}"));\n'
        )
        conversion = rust_to_idl_value(member.type, member.extended_attributes, "member", member.name, context, what)
        if member.required:
            missing = f"""return vm.throw_completion(
                    ErrorKind::TypeError,
                    ErrorType::MissingRequiredProperty,
                    &[&"{member.name}"],
                )"""
            present = f"{conversion}?"
        elif member.default_value is not None:
            missing = rust_default_value(member, context, what)
            present = f"{conversion}?"
            empty_fields.append(f"{rust_identifier(member.name)}: {missing}")
        else:
            missing = "None"
            present = f"Some({conversion}?)"
            empty_fields.append(f"{rust_identifier(member.name)}: None")
        conversions.append(
            f"""            let {local} = match dictionary_member(vm, object, &{key})? {{
                Some(member) => {present},
                None => {missing},
            }};
"""
        )
    fields = ", ".join(
        f"{rust_identifier(member.name)}: {rust_local_name(member.name)}" for member in dictionary.members
    )
    out.write(
        f"""
    /// The conversion of a JS value to the dictionary {dictionary.name}, https://webidl.spec.whatwg.org/#js-dictionary
    impl FromJs for {path} {{
        fn from_js(vm: &Vm, value: Value) -> ThrowCompletionOr<Self> {{
{"".join(keys)}            let object = dictionary_object(vm, value, "{dictionary.name}")?;
{"".join(conversions)}            Ok(Self {{ {fields} }})
        }}
    }}
"""
    )
    if dictionary_can_be_empty(dictionary):
        out.write(
            f"""
    impl EmptyDictionary for {path} {{
        fn empty() -> Self {{
            Self {{ {", ".join(empty_fields)} }}
        }}
    }}
"""
        )


def write_union_from_js(out: TextIO, context: GenerationContext, union_type: IDLUnionType) -> None:
    """The conversion of a JS value to the enum of a union, with the steps of https://webidl.spec.whatwg.org/#js-union
    that apply to its members, in the order of the C++ bindings."""
    what = f"the union {union_type}"
    path = rust_union_path(union_type, context, what)
    dictionary_type, string_type = union_members_by_kind(union_type, context, what)
    steps = []
    if dictionary_type is not None:
        dictionary_path = rust_dictionary_path(context.dictionaries[dictionary_type.name], context)
        steps.append(
            f"""            // 3. If V is null or undefined, then:
            //     1. If types includes a dictionary type, then return the result of converting V to that
            //        dictionary type.
            // 13. If V is an Object, then:
            //     4. If types includes a dictionary type, then return the result of converting V to that
            //        dictionary type.
            if value.is_nullish() || value.is_object() {{
                return Ok(Self::{rust_union_variant(dictionary_type)}(<{dictionary_path}>::from_js(vm, value)?));
            }}
"""
        )
    if string_type is not None:
        conversion = rust_to_idl_value(string_type, {}, "value", "value", context, what)
        steps.append(
            f"""            // 17. If types includes a string type, then return the result of converting V to that type.
            Ok(Self::{rust_union_variant(string_type)}({conversion}?))
"""
        )
    else:
        steps.append(
            """            // 19. Throw a TypeError.
            vm.throw_completion_with_message(ErrorKind::TypeError, "No union types matched".to_string())
"""
        )
    out.write(
        f"""
    /// The conversion of a JS value to the union {union_type}, https://webidl.spec.whatwg.org/#js-union
    impl FromJs for {path} {{
        fn from_js(vm: &Vm, value: Value) -> ThrowCompletionOr<Self> {{
{"".join(steps)}        }}
    }}
"""
    )
