# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

# The Rust expressions that convert JS values to IDL values, https://webidl.spec.whatwg.org/#js-type-mapping, for the
# Rust bindings of the interfaces implemented in Rust, as to_idl_value.py writes the C++ ones. The conversions run in
# the order of the C++ bindings and throw their errors, so that an interface behaves the same in either language.

from dataclasses import dataclass
from dataclasses import field
from typing import List
from typing import Optional
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
from Generators.libweb_bindings.rust_types import cpp_interface
from Generators.libweb_bindings.rust_types import cpp_interface_name_constant
from Generators.libweb_bindings.rust_types import holds_cells
from Generators.libweb_bindings.rust_types import is_named_union
from Generators.libweb_bindings.rust_types import member_other_than_undefined
from Generators.libweb_bindings.rust_types import resolved_dictionary
from Generators.libweb_bindings.rust_types import runtime_union_type
from Generators.libweb_bindings.rust_types import rust_dictionary_path
from Generators.libweb_bindings.rust_types import rust_enumeration_path
from Generators.libweb_bindings.rust_types import rust_enumeration_variant
from Generators.libweb_bindings.rust_types import rust_integer_literal
from Generators.libweb_bindings.rust_types import rust_interface
from Generators.libweb_bindings.rust_types import rust_interface_path
from Generators.libweb_bindings.rust_types import rust_type
from Generators.libweb_bindings.rust_types import rust_union_path
from Generators.libweb_bindings.rust_types import rust_union_variant
from Utils.webidl_parser import Dictionary
from Utils.webidl_parser import DictionaryMember
from Utils.webidl_parser import Enumeration
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
    if isinstance(idl_type, IDLParameterizedType) and idl_type.name == "sequence":
        element = rust_sequence_element_to_idl_value(idl_type, identifier, context, what)
        return f"sequence_from_js(vm, {value}, |value| {element})"
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
    if runtime_union_type(idl_type, context) == "ArrayBufferView":
        return f"to_array_buffer_view(vm, {value}, {buffer_source_rules(idl_type, parameter_attributes)})"
    enumeration = context.enumeration(idl_type)
    if enumeration is not None:
        return f"<{rust_enumeration_path(enumeration, context)}>::from_js(vm, {value})"
    interface = rust_interface(idl_type, context)
    if interface is not None:
        return f"to_interface::<{rust_interface_path(interface)}>(vm, {value})"
    interface = cpp_interface(idl_type, context)
    if interface is not None:
        return f'to_cpp_interface(vm, {value}, {cpp_interface_name_constant(interface)}, "{interface.name}")'
    dictionary = resolved_dictionary(idl_type, context)
    if dictionary is not None:
        return f"<{rust_dictionary_path(dictionary, context)}>::from_js(vm, {value})"
    if isinstance(idl_type, IDLUnionType) and is_named_union(idl_type, context):
        union_members_by_kind(idl_type, context, what)
        # NB: The conversion of a named union is written once, for every parameter of its type.
        allowances = sorted(set(parameter_attributes) & {"AllowShared", "AllowResizable"})
        if allowances:
            raise UnsupportedByRustBindings(
                f"{what} has the extended attributes {allowances} on the union type {idl_type}, which the Rust "
                "bindings only handle on the members of the union"
            )
        return f"<{rust_union_path(idl_type, context, what)}>::from_js(vm, {value})"
    raise UnsupportedByRustBindings(f"{what} has the type {idl_type}, which the Rust bindings do not convert to yet")


def rust_sequence_element_to_idl_value(
    sequence_type: IDLParameterizedType, identifier: str, context: GenerationContext, what: str
) -> str:
    """The conversion of the JS value `value` to an element of the sequence type."""
    element_type = sequence_type.parameters[0]
    if holds_cells(element_type, context):
        # NB: The elements are gathered in a Vec, which hides cells from the collector while the rest are converted.
        raise UnsupportedByRustBindings(
            f"{what} has the type {sequence_type}, a sequence of values that are or can hold cells, which the Rust "
            "bindings do not convert to yet"
        )
    return rust_to_idl_value(element_type, {}, "value", identifier, context, what)


@dataclass
class UnionMembers:
    """The members of a union by the kinds that the Rust bindings distinguish between when they convert a JS value to
    the union."""

    dictionary_type: Optional[IDLType] = None
    string_type: Optional[IDLType] = None
    sequence_type: Optional[IDLParameterizedType] = None
    typed_array_types: List[IDLType] = field(default_factory=list)
    cpp_interface_types: List[IDLType] = field(default_factory=list)


def union_members_by_kind(union_type: IDLUnionType, context: GenerationContext, what: str) -> UnionMembers:
    members = UnionMembers()
    for member_type in union_type.flattened_member_types():
        if resolved_dictionary(member_type, context) is not None and members.dictionary_type is None:
            members.dictionary_type = member_type
        elif member_type.name in STRING_TYPES and members.string_type is None:
            members.string_type = member_type
        elif member_type.name in TYPED_ARRAY_NAMES:
            members.typed_array_types.append(member_type)
        elif cpp_interface(member_type, context) is not None:
            members.cpp_interface_types.append(member_type)
        elif (
            isinstance(member_type, IDLParameterizedType)
            and member_type.name == "sequence"
            and members.sequence_type is None
        ):
            members.sequence_type = member_type
        else:
            raise UnsupportedByRustBindings(
                f"{what} has the union type {union_type}, and the Rust bindings do not convert a JS value to a "
                f"union with the member {member_type} yet"
            )
    return members


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
    enumeration = context.enumeration(member.type)
    if enumeration is not None and default_value is not None and default_value[1:-1] in enumeration.values:
        variant = rust_enumeration_variant(default_value[1:-1], enumeration)
        return f"{rust_enumeration_path(enumeration, context)}::{variant}"
    if isinstance(member.type, IDLUnionType) and is_named_union(member.type, context) and default_value == "{}":
        dictionary_type = union_members_by_kind(member.type, context, what).dictionary_type
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


def write_enumeration_from_js(out: TextIO, context: GenerationContext, enumeration: Enumeration) -> None:
    """The conversion of a JS value to the enum of an enumeration, which the module of its IDL file defines with a
    variant for each value, named as the C++ bindings name theirs."""
    values = ", ".join(f'"{value}"' for value in enumeration.values)
    arms = "".join(
        f"                {index} => Self::{rust_enumeration_variant(value, enumeration)},\n"
        for index, value in enumerate(enumeration.values)
    )
    out.write(
        f"""
    /// The conversion of a JS value to the enumeration {enumeration.name},
    /// https://webidl.spec.whatwg.org/#js-enumeration
    impl FromJs for {rust_enumeration_path(enumeration, context)} {{
        fn from_js(vm: &Vm, value: Value) -> ThrowCompletionOr<Self> {{
            Ok(match to_enumeration_value(vm, value, "{enumeration.name}", &[{values}])? {{
{arms}                _ => unreachable!("the value is one of the values of the enumeration"),
            }})
        }}
    }}
"""
    )


def write_union_from_js(out: TextIO, context: GenerationContext, union_type: IDLUnionType) -> None:
    """The conversion of a JS value to the enum of a union, with the steps of https://webidl.spec.whatwg.org/#js-union
    that apply to its members, in the order of the C++ bindings."""
    what = f"the union {union_type}"
    path = rust_union_path(union_type, context, what)
    members = union_members_by_kind(union_type, context, what)
    dictionary_type = members.dictionary_type
    string_type = members.string_type
    steps = []
    dictionary_conversion = None
    if dictionary_type is not None:
        dictionary_path = rust_dictionary_path(context.dictionaries[dictionary_type.name], context)
        dictionary_conversion = (
            f"Ok(Self::{rust_union_variant(dictionary_type)}(<{dictionary_path}>::from_js(vm, value)?))"
        )
    if members.typed_array_types or members.sequence_type is not None or members.cpp_interface_types:
        if dictionary_conversion is not None:
            steps.append(
                f"""            // 3. If V is null or undefined, then:
            //     1. If types includes a dictionary type, then return the result of converting V to that
            //        dictionary type.
            if value.is_nullish() {{
                return {dictionary_conversion};
            }}
"""
            )
        steps.append("            if value.is_object() {\n")
        for interface_type in members.cpp_interface_types:
            interface = context.interfaces[interface_type.name]
            steps.append(
                f"""                // 5. If V is a platform object, then:
                //     1. If types includes an interface type that V implements, then return the IDL value that is a
                //        reference to the object V.
                if implements_cpp_interface(value.as_object(), {cpp_interface_name_constant(interface)}) {{
                    // SAFETY: The object is a wrapper of an object that implements the interface.
                    let object = unsafe {{ CppInterface::from_wrapper(value.as_object()) }};
                    return Ok(Self::{rust_union_variant(interface_type)}(object));
                }}
"""
            )
        for typed_array_type in members.typed_array_types:
            name = typed_array_type.name
            conversion = rust_to_idl_value(typed_array_type, {}, "value", "value", context, what)
            steps.append(
                f"""                // 9. If V is an Object and V has a [[TypedArrayName]] internal slot, then:
                //     1. If types includes a typed array type whose name is the value of V's [[TypedArrayName]]
                //        internal slot, then return the result of converting V to that type.
                let typed_array = value.as_object().downcast::<TypedArrayBase>();
                if typed_array.is_some_and(|typed_array| typed_array.kind() == TypedArrayKind::{name}) {{
                    return Ok(Self::{rust_union_variant(typed_array_type)}({conversion}?));
                }}
"""
            )
        if members.sequence_type is not None:
            element = rust_sequence_element_to_idl_value(members.sequence_type, "value", context, what)
            steps.append(
                f"""                // 11. If V is an Object, then:
                //     2. If types includes a sequence type, then:
                //         1. Let method be ? GetMethod(V, %Symbol.iterator%).
                //         2. If method is not undefined, return the result of creating a sequence of that type from
                //            V and method.
                if let Some(method) = iterator_method(vm, value)? {{
                    return Ok(Self::Sequence(sequence_from_iterable(vm, value, method, |value| {element})?));
                }}
"""
            )
        if dictionary_conversion is not None:
            steps.append(
                f"""                //     4. If types includes a dictionary type, then return the result of converting V to that
                //        dictionary type.
                return {dictionary_conversion};
"""
            )
        steps.append("            }\n")
    elif dictionary_conversion is not None:
        steps.append(
            f"""            // 3. If V is null or undefined, then:
            //     1. If types includes a dictionary type, then return the result of converting V to that
            //        dictionary type.
            // 13. If V is an Object, then:
            //     4. If types includes a dictionary type, then return the result of converting V to that
            //        dictionary type.
            if value.is_nullish() || value.is_object() {{
                return {dictionary_conversion};
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
            throw_no_union_types_matched(vm)
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
