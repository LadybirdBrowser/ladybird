# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

# The choice between the overloads of a constructor or of an operation of an interface implemented in Rust, with the
# overload resolution algorithm, https://webidl.spec.whatwg.org/#dfn-overload-resolution-algorithm
#
# The C++ bindings pass the effective overload set to WebIDL::resolve_overload() of
# Libraries/LibWeb/WebIDL/OverloadResolution.cpp, which tests the argument at the distinguishing index against the
# types of the overloads when it runs. The types are known here, so the Rust bindings get only the tests that can
# select an overload, in the order of that function and with its results, including where it differs from the
# algorithm. The steps below follow it one by one; a change to either has to be made to both.

from dataclasses import dataclass
from typing import Callable
from typing import List
from typing import Optional
from typing import Sequence
from typing import TextIO
from typing import Union

from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.overload_resolution import EffectiveOverloadItem
from Generators.libweb_bindings.overload_resolution import Optionality
from Generators.libweb_bindings.overload_resolution import compute_the_effective_overload_set
from Generators.libweb_bindings.overload_resolution import resolve_distinguishing_argument_index
from Generators.libweb_bindings.rust_types import TYPED_ARRAY_NAMES
from Generators.libweb_bindings.rust_types import UnsupportedByRustBindings
from Generators.libweb_bindings.rust_types import cpp_interface
from Generators.libweb_bindings.rust_types import cpp_interface_name_constant
from Generators.libweb_bindings.rust_types import rust_interface
from Generators.libweb_bindings.rust_types import rust_interface_path
from Utils.webidl_parser import Constructor
from Utils.webidl_parser import IDLParameterizedType
from Utils.webidl_parser import IDLType
from Utils.webidl_parser import IDLUnionType
from Utils.webidl_parser import Interface
from Utils.webidl_parser import Operation

STRING_TYPES = {"ByteString", "DOMString", "USVString"}
INTEGER_TYPES = {
    "byte",
    "octet",
    "short",
    "unsigned short",
    "long",
    "unsigned long",
    "long long",
    "unsigned long long",
}
FLOATING_POINT_TYPES = {"float", "double", "unrestricted float", "unrestricted double"}


# What an entry of the effective overload set makes a step do: nothing, select the entry, or select it if a
# condition on the argument holds, which is a Rust expression.
NO_MATCH = None
ALWAYS = ""


@dataclass
class OverloadResolutionStep:
    description: str
    # The test of the argument at the distinguishing index, or None for a step that every argument reaches.
    argument_test: Optional[str]
    # Whether an entry of the effective overload set with the type and the optionality at the distinguishing index
    # makes the step select the entry: NO_MATCH, ALWAYS, or the condition on the argument under which it does.
    entry_condition: Callable[[IDLType, Optionality], Optional[str]]


def is_plain(idl_type: IDLType) -> bool:
    return not isinstance(idl_type, (IDLUnionType, IDLParameterizedType))


def type_and_flattened_members(idl_type: IDLType) -> List[IDLType]:
    """The types that has_overload_with_argument_type_or_subtype_matching() of OverloadResolution.cpp tests for one
    entry."""
    if isinstance(idl_type, IDLUnionType):
        return [idl_type, *idl_type.flattened_member_types()]
    return [idl_type]


def type_or_flattened_member_matches(idl_type: IDLType, matches: Callable[[IDLType], bool]) -> bool:
    return any(matches(member_type) for member_type in type_and_flattened_members(idl_type))


def overload_resolution_steps(context: GenerationContext) -> List[OverloadResolutionStep]:
    """The sub-steps of step 12 of the algorithm that resolve_overload() runs, in its order, without the ones for
    functions and sequences and for the type object, which check_type_at_distinguishing_index() keeps out."""

    def is_dictionary(idl_type: IDLType) -> bool:
        return is_plain(idl_type) and context.dictionary(idl_type) is not None

    def is_string(idl_type: IDLType) -> bool:
        return is_plain(idl_type) and (idl_type.name in STRING_TYPES or context.enumeration(idl_type) is not None)

    def is_numeric(idl_type: IDLType) -> bool:
        return is_plain(idl_type) and idl_type.name in INTEGER_TYPES | FLOATING_POINT_TYPES

    def is_named(name: str) -> Callable[[IDLType], bool]:
        return lambda idl_type: is_plain(idl_type) and idl_type.name == name

    def nullable_or_dictionary(idl_type: IDLType) -> bool:
        if idl_type.nullable or is_dictionary(idl_type):
            return True
        # NB: resolve_overload() asks whether the union is a dictionary for each of its members, where the algorithm
        #     asks whether the member is one, so a dictionary inside a union does not match here.
        if isinstance(idl_type, IDLUnionType):
            return any(member_type.nullable for member_type in idl_type.flattened_member_types())
        return False

    def when(matches: bool) -> Optional[str]:
        return ALWAYS if matches else NO_MATCH

    def with_members(matches: Callable[[IDLType], bool]) -> Callable[[IDLType, Optionality], Optional[str]]:
        return lambda idl_type, _: when(type_or_flattened_member_matches(idl_type, matches))

    def implemented_interface(idl_type: IDLType, _: Optionality) -> Optional[str]:
        """The condition under which the platform object that the argument is implements an interface of the type."""
        tests = []
        for member_type in type_and_flattened_members(idl_type):
            if not is_plain(member_type):
                continue
            interface = cpp_interface(member_type, context)
            if interface is not None:
                tests.append(f"implements_cpp_interface(value.as_object(), {cpp_interface_name_constant(interface)})")
            interface = rust_interface(member_type, context)
            if interface is not None:
                wrapper_class = f"<{rust_interface_path(interface)}>::wrapper_class()"
                tests.append(f"is_host_instance_of(&value.as_object(), {wrapper_class})")
        return "\n                        || ".join(tests) if tests else NO_MATCH

    def typed_array_of_kind(idl_type: IDLType, _: Optionality) -> Optional[str]:
        """The condition under which the typed array that the argument is has the name of a typed array type of the
        type."""
        names = [
            member_type.name
            for member_type in type_and_flattened_members(idl_type)
            if is_plain(member_type) and member_type.name in TYPED_ARRAY_NAMES
        ]
        if not names:
            return NO_MATCH
        if set(names) == TYPED_ARRAY_NAMES:
            return ALWAYS
        kinds = " | ".join(f"TypedArrayKind::{name}" for name in names)
        return (
            "value.as_object().downcast::<TypedArrayBase>()"
            f".is_some_and(|typed_array| matches!(typed_array.kind(), {kinds}))"
        )

    return [
        OverloadResolutionStep(
            "The argument is undefined, and an overload has an optional argument there.",
            "value.is_undefined()",
            lambda _, optionality: when(optionality == Optionality.Optional),
        ),
        OverloadResolutionStep(
            "The argument is null or undefined, and an overload takes a nullable type or a dictionary there.",
            "value.is_nullish()",
            lambda idl_type, _: when(nullable_or_dictionary(idl_type)),
        ),
        OverloadResolutionStep(
            "The argument is a platform object, and an overload takes an interface that it implements there.",
            "value.is_object() && value.as_object().is_platform_object()",
            implemented_interface,
        ),
        # NB: resolve_overload() takes a SharedArrayBuffer for an ArrayBuffer here, which the conversion of the
        #     argument then rejects.
        OverloadResolutionStep(
            "The argument is an ArrayBuffer, and an overload takes one there.",
            "value.is_object() && value.as_object().downcast::<ArrayBuffer>().is_some()",
            with_members(is_named("ArrayBuffer")),
        ),
        OverloadResolutionStep(
            "The argument is a DataView, and an overload takes one there.",
            "value.is_object() && value.as_object().downcast::<DataView>().is_some()",
            with_members(is_named("DataView")),
        ),
        OverloadResolutionStep(
            "The argument is a typed array, and an overload takes one with its name there.",
            "value.is_object() && value.as_object().downcast::<TypedArrayBase>().is_some()",
            typed_array_of_kind,
        ),
        OverloadResolutionStep(
            "The argument is an object, and an overload takes a dictionary there.",
            "value.is_object()",
            with_members(is_dictionary),
        ),
        OverloadResolutionStep(
            "The argument is a Boolean, and an overload takes a boolean there.",
            "value.is_boolean()",
            with_members(is_named("boolean")),
        ),
        OverloadResolutionStep(
            "The argument is a Number, and an overload takes a numeric type there.",
            "value.is_number()",
            with_members(is_numeric),
        ),
        OverloadResolutionStep(
            "The argument is a BigInt, and an overload takes a bigint there.",
            "value.is_bigint()",
            with_members(is_named("bigint")),
        ),
        OverloadResolutionStep(
            "An overload takes a string type there.",
            None,
            with_members(is_string),
        ),
        OverloadResolutionStep(
            "An overload takes a numeric type there.",
            None,
            with_members(is_numeric),
        ),
        OverloadResolutionStep(
            "An overload takes a boolean there.",
            None,
            with_members(is_named("boolean")),
        ),
        OverloadResolutionStep(
            "An overload takes a bigint there.",
            None,
            with_members(is_named("bigint")),
        ),
        OverloadResolutionStep(
            "An overload takes any there.",
            None,
            lambda idl_type, _: when(is_named("any")(idl_type)),
        ),
    ]


def check_type_at_distinguishing_index(idl_type: IDLType, context: GenerationContext, what: str) -> None:
    """Rejects the types that the steps of resolve_overload() which are not written out here would take an argument
    for: those for functions and sequences, and the type object, which every step for an object takes."""
    member_types = idl_type.flattened_member_types() if isinstance(idl_type, IDLUnionType) else [idl_type]
    for member_type in member_types:
        handled = is_plain(member_type) and (
            context.dictionary(member_type) is not None
            or context.enumeration(member_type) is not None
            or cpp_interface(member_type, context) is not None
            or rust_interface(member_type, context) is not None
            or member_type.name in STRING_TYPES | INTEGER_TYPES | FLOATING_POINT_TYPES | TYPED_ARRAY_NAMES
            or member_type.name in ("boolean", "bigint", "any", "ArrayBuffer", "DataView")
        )
        if not handled:
            raise UnsupportedByRustBindings(
                f"{what} are told apart by an argument of the type {idl_type}, and the Rust bindings do not resolve "
                f"overloads by the type {member_type} yet"
            )


def write_resolution_at_distinguishing_index(
    out: TextIO,
    context: GenerationContext,
    interface: Interface,
    entries: List[EffectiveOverloadItem],
    argument_count: int,
    call_for: Callable[[int], str],
    what: str,
) -> None:
    index = resolve_distinguishing_argument_index(interface, entries, argument_count, context)
    for entry in entries:
        check_type_at_distinguishing_index(entry.types[index], context, what)
    out.write(f"                let value = vm.argument({index});\n")
    for step in overload_resolution_steps(context):
        # NB: The first entry that a step matches is the one that it keeps. An entry that only matches under a
        #     condition on the argument leaves the entries after it to the step when the condition does not hold.
        selections = []
        for entry in entries:
            condition = step.entry_condition(entry.types[index], entry.optionality_values[index])
            if condition is NO_MATCH:
                continue
            selections.append((condition, call_for(entry.callable_id)))
            if condition == ALWAYS:
                break
        if not selections:
            continue
        if step.argument_test is None:
            assert selections[0][0] == ALWAYS
            out.write(f"                // {step.description}\n                {selections[0][1]}\n")
            return
        if selections[0][0] == ALWAYS:
            out.write(
                f"""                // {step.description}
                if {step.argument_test} {{
                    return {selections[0][1]};
                }}
"""
            )
            continue
        if len(selections) == 1:
            condition, call = selections[0]
            condition = f"({condition})" if "||" in condition else condition
            out.write(
                f"""                // {step.description}
                if {step.argument_test} && {condition} {{
                    return {call};
                }}
"""
            )
            continue
        out.write(f"                // {step.description}\n                if {step.argument_test} {{\n")
        for condition, call in selections:
            if condition == ALWAYS:
                out.write(f"                    return {call};\n")
            else:
                out.write(
                    f"""                    if {condition} {{
                        return {call};
                    }}
"""
                )
        out.write("                }\n")
    out.write("                vm.throw_completion(ErrorKind::TypeError, ErrorType::OverloadResolutionFailed, &[])\n")


def write_overload_resolution(
    out: TextIO,
    context: GenerationContext,
    interface: Interface,
    overloads: Sequence[Union[Constructor, Operation]],
    signature: str,
    documentation: str,
    call_for: Callable[[int], str],
    what: str,
) -> None:
    """Writes the function with the signature that chooses between the overloads for the arguments of a call.
    `call_for` gives the call of the steps of the overload at a position among the overloads."""
    maximum_argument_count = 0
    entries_by_argument_count: dict[int, List[EffectiveOverloadItem]] = {}
    for entry in compute_the_effective_overload_set(overloads):
        maximum_argument_count = max(maximum_argument_count, len(entry.types))
        entries_by_argument_count.setdefault(len(entry.types), []).append(entry)

    out.write(
        f"""
{documentation}    {signature} {{
        match vm.argument_count().min({maximum_argument_count}) {{
"""
    )
    for argument_count, entries in sorted(entries_by_argument_count.items()):
        if len(entries) == 1:
            out.write(f"            {argument_count} => {call_for(entries[0].callable_id)},\n")
            continue
        out.write(f"            {argument_count} => {{\n")
        write_resolution_at_distinguishing_index(out, context, interface, entries, argument_count, call_for, what)
        out.write("            }\n")
    out.write(
        """            _ => vm.throw_completion(ErrorKind::TypeError, ErrorType::OverloadResolutionFailed, &[]),
        }
    }
"""
    )


def write_constructor_overload_resolution(
    out: TextIO, context: GenerationContext, interface: Interface, steps_functions: List[str]
) -> None:
    """Writes the function that chooses the constructor for the arguments of a construction. The function of the
    steps of each constructor is named in the order of the constructors of the interface."""
    write_overload_resolution(
        out,
        context,
        interface,
        interface.constructors,
        "fn construct(vm: &Vm, new_target: Gc<Object>) -> ThrowCompletionOr<Value>",
        """    /// Chooses the constructor for the arguments of a construction, with the steps of the overload resolution
    /// algorithm that can select one of the constructors of the interface,
    /// https://webidl.spec.whatwg.org/#dfn-overload-resolution-algorithm
""",
        lambda callable_id: f"{steps_functions[callable_id]}(vm, new_target)",
        f"The constructors of {interface.name}",
    )


def write_operation_overload_resolution(
    out: TextIO,
    context: GenerationContext,
    interface: Interface,
    name: str,
    function: str,
    operations: List[Operation],
    steps_functions: List[str],
) -> None:
    """Writes the native function of an overloaded operation, which chooses the overload for the arguments of a call.
    The function of the steps of each overload is named in the order of the overloads. Like the C++ bindings, it does
    not look at the this value, which the steps of the chosen overload check."""
    write_overload_resolution(
        out,
        context,
        interface,
        operations,
        f"fn {function}(vm: &Vm) -> ThrowCompletionOr<Value>",
        f"""    /// Chooses the overload of {name}() for the arguments of a call, with the steps of the overload resolution
    /// algorithm that can select one of them, https://webidl.spec.whatwg.org/#dfn-overload-resolution-algorithm
""",
        lambda callable_id: f"{steps_functions[callable_id]}(vm)",
        f"The overloads of {interface.name}.{name}()",
    )
