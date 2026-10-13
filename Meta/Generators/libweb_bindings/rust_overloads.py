# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

# The choice between the overloads of a constructor of an interface implemented in Rust, with the overload resolution
# algorithm, https://webidl.spec.whatwg.org/#dfn-overload-resolution-algorithm
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
from typing import TextIO

from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.overload_resolution import EffectiveOverloadItem
from Generators.libweb_bindings.overload_resolution import Optionality
from Generators.libweb_bindings.overload_resolution import compute_the_effective_overload_set
from Generators.libweb_bindings.overload_resolution import resolve_distinguishing_argument_index
from Generators.libweb_bindings.rust_types import UnsupportedByRustBindings
from Utils.webidl_parser import IDLParameterizedType
from Utils.webidl_parser import IDLType
from Utils.webidl_parser import IDLUnionType
from Utils.webidl_parser import Interface

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


@dataclass
class OverloadResolutionStep:
    description: str
    # The test of the argument at the distinguishing index, or None for a step that every argument reaches.
    argument_test: Optional[str]
    # Whether an entry of the effective overload set with the type and the optionality at the distinguishing index
    # makes the step select the entry.
    entry_matches: Callable[[IDLType, Optionality], bool]


def is_plain(idl_type: IDLType) -> bool:
    return not isinstance(idl_type, (IDLUnionType, IDLParameterizedType))


def type_or_flattened_member_matches(idl_type: IDLType, matches: Callable[[IDLType], bool]) -> bool:
    """has_overload_with_argument_type_or_subtype_matching() of OverloadResolution.cpp, for one entry."""
    if matches(idl_type):
        return True
    if isinstance(idl_type, IDLUnionType):
        return any(matches(member_type) for member_type in idl_type.flattened_member_types())
    return False


def overload_resolution_steps(context: GenerationContext) -> List[OverloadResolutionStep]:
    """The sub-steps of step 12 of the algorithm that resolve_overload() runs when the argument at the distinguishing
    index is not an object that a step for interfaces, buffers, functions or sequences would take, in its order."""

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

    def with_members(matches: Callable[[IDLType], bool]) -> Callable[[IDLType, Optionality], bool]:
        return lambda idl_type, _: type_or_flattened_member_matches(idl_type, matches)

    return [
        OverloadResolutionStep(
            "The argument is undefined, and an overload has an optional argument there.",
            "value.is_undefined()",
            lambda _, optionality: optionality == Optionality.Optional,
        ),
        OverloadResolutionStep(
            "The argument is null or undefined, and an overload takes a nullable type or a dictionary there.",
            "value.is_nullish()",
            lambda idl_type, _: nullable_or_dictionary(idl_type),
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
            lambda idl_type, _: is_named("any")(idl_type),
        ),
    ]


def check_type_at_distinguishing_index(idl_type: IDLType, context: GenerationContext, what: str) -> None:
    """Rejects the types that the steps of resolve_overload() which are not written out here would take an argument
    for: those for platform objects, buffers, functions and sequences, which test the object that the argument is."""
    member_types = idl_type.flattened_member_types() if isinstance(idl_type, IDLUnionType) else [idl_type]
    for member_type in member_types:
        handled = is_plain(member_type) and (
            context.dictionary(member_type) is not None
            or context.enumeration(member_type) is not None
            or member_type.name in STRING_TYPES | INTEGER_TYPES | FLOATING_POINT_TYPES
            or member_type.name in ("boolean", "bigint", "any")
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
    steps_functions: List[str],
    what: str,
) -> None:
    index = resolve_distinguishing_argument_index(interface, entries, argument_count, context)
    for entry in entries:
        check_type_at_distinguishing_index(entry.types[index], context, what)
    out.write(f"                let value = vm.argument({index});\n")
    for step in overload_resolution_steps(context):
        # NB: The first entry that a step matches is the one that it keeps.
        selected = next(
            (entry for entry in entries if step.entry_matches(entry.types[index], entry.optionality_values[index])),
            None,
        )
        if selected is None:
            continue
        call = f"{steps_functions[selected.callable_id]}(vm, new_target)"
        if step.argument_test is None:
            out.write(f"                // {step.description}\n                {call}\n")
            return
        out.write(
            f"""                // {step.description}
                if {step.argument_test} {{
                    return {call};
                }}
"""
        )
    out.write("                vm.throw_completion(ErrorKind::TypeError, ErrorType::OverloadResolutionFailed, &[])\n")


def write_constructor_overload_resolution(
    out: TextIO, context: GenerationContext, interface: Interface, steps_functions: List[str]
) -> None:
    """Writes the function that chooses the constructor for the arguments of a construction. The function of the
    steps of each constructor is named in the order of the constructors of the interface."""
    what = f"The constructors of {interface.name}"
    maximum_argument_count = 0
    entries_by_argument_count: dict[int, List[EffectiveOverloadItem]] = {}
    for entry in compute_the_effective_overload_set(interface.constructors):
        maximum_argument_count = max(maximum_argument_count, len(entry.types))
        entries_by_argument_count.setdefault(len(entry.types), []).append(entry)

    out.write(
        f"""
    /// Chooses the constructor for the arguments of a construction, with the steps of the overload resolution
    /// algorithm that can select one of the constructors of the interface,
    /// https://webidl.spec.whatwg.org/#dfn-overload-resolution-algorithm
    fn construct(vm: &Vm, new_target: Gc<Object>) -> ThrowCompletionOr<Value> {{
        match vm.argument_count().min({maximum_argument_count}) {{
"""
    )
    for argument_count, entries in sorted(entries_by_argument_count.items()):
        if len(entries) == 1:
            out.write(f"            {argument_count} => {steps_functions[entries[0].callable_id]}(vm, new_target),\n")
            continue
        out.write(f"            {argument_count} => {{\n")
        write_resolution_at_distinguishing_index(
            out, context, interface, entries, argument_count, steps_functions, what
        )
        out.write("            }\n")
    out.write(
        """            _ => vm.throw_completion(ErrorKind::TypeError, ErrorType::OverloadResolutionFailed, &[]),
        }
    }
"""
    )
