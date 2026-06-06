# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from dataclasses import dataclass
from enum import Enum
from typing import Optional
from typing import Sequence
from typing import TextIO
from typing import Union

from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.cpp_types import idl_identifier_cpp_name
from Generators.libweb_bindings.cpp_types import is_numeric_type
from Generators.libweb_bindings.cpp_types import is_string_type
from Generators.libweb_bindings.includes import GeneratedIncludes
from Utils.webidl_parser import Constructor
from Utils.webidl_parser import IDLParameterizedType
from Utils.webidl_parser import IDLType
from Utils.webidl_parser import IDLUnionType
from Utils.webidl_parser import Interface
from Utils.webidl_parser import Operation
from Utils.webidl_parser import OperationParameter


class Optionality(Enum):
    Required = "Required"
    Optional = "Optional"
    Variadic = "Variadic"


@dataclass
class EffectiveOverloadItem:
    callable_id: int
    types: list[IDLType]
    optionality_values: list[Optionality]


def write_overload_resolution_switch(
    out: TextIO,
    context: GenerationContext,
    interface: Interface,
    overloads: Sequence[Union[Constructor, Operation]],
) -> None:
    maximum_argument_count = 0
    effective_overload_sets: dict[int, list[EffectiveOverloadItem]] = {}
    for overload in compute_the_effective_overload_set(overloads):
        maximum_argument_count = max(maximum_argument_count, len(overload.types))
        effective_overload_sets.setdefault(len(overload.types), []).append(overload)

    dictionary_types: set[str] = set()
    out.write(
        f"""    Optional<int> chosen_overload_callable_id;
    Optional<WebIDL::EffectiveOverloadSet> effective_overload_set;

    switch (min({maximum_argument_count}, vm.argument_count())) {{
"""
    )

    for argument_count, effective_overload_set in sorted(effective_overload_sets.items()):
        if len(effective_overload_set) == 1:
            overload = effective_overload_set[0]
            dictionary_types.update(context.dictionary_type_names(*overload.types))
            out.write(
                f"""    case {argument_count}:
        chosen_overload_callable_id = {overload.callable_id};
        break;
"""
            )
            continue

        distinguishing_argument_index = resolve_distinguishing_argument_index(
            interface,
            effective_overload_set,
            argument_count,
            context,
        )
        out.write(
            f"""    case {argument_count}: {{
        Vector<WebIDL::EffectiveOverloadSet::Item> overloads;
        overloads.ensure_capacity({len(effective_overload_set)});
"""
        )
        for overload in effective_overload_set:
            dictionary_types.update(context.dictionary_type_names(*overload.types))
            types = ", ".join(constructor_for_idl_type(idl_type, context) for idl_type in overload.types)
            optionality_values = ", ".join(
                f"WebIDL::Optionality::{optionality.value}" for optionality in overload.optionality_values
            )
            out.write(
                f"""        overloads.empend({overload.callable_id}, Vector<NonnullRefPtr<WebIDL::Type const>> {{ {types} }}, Vector<WebIDL::Optionality> {{ {optionality_values} }});
"""
            )
        out.write(
            f"""        effective_overload_set.emplace(move(overloads), {distinguishing_argument_index});
        break;
    }}
"""
        )

    out.write("""    }

    Vector<StringView> dictionary_types {
""")
    for dictionary_type in sorted(dictionary_types):
        out.write(f'        "{dictionary_type}"sv,\n')
    out.write(
        """    };

    if (!chosen_overload_callable_id.has_value()) {
        if (!effective_overload_set.has_value())
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::OverloadResolutionFailed);
        chosen_overload_callable_id = TRY(WebIDL::resolve_overload(vm, effective_overload_set.value(), dictionary_types)).callable_id;
    }

"""
    )


def write_overload_arbiter(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
    operations: list[Operation],
    receiver_class: Optional[str] = None,
) -> None:
    if receiver_class is None:
        receiver_class = interface.prototype_class

    includes.add("AK/Optional.h")
    includes.add("AK/Vector.h")
    includes.add("LibWeb/WebIDL/OverloadTypes.h")
    includes.add("LibWeb/WebIDL/OverloadResolution.h")
    includes.add("LibWeb/WebIDL/Tracing.h")

    operation = operations[0]
    out.write(
        f"""JS_DEFINE_NATIVE_FUNCTION({receiver_class}::{idl_identifier_cpp_name(operation)})
{{
    WebIDL::log_trace(vm, "{receiver_class}::{idl_identifier_cpp_name(operation)}");

"""
    )
    write_overload_resolution_switch(out, context, interface, operations)
    out.write(
        """
    switch (chosen_overload_callable_id.value()) {
"""
    )
    for overload_index, overload in enumerate(operations):
        out.write(
            f"""    case {overload_index}:
        return {idl_identifier_cpp_name(overload, suffix=overload_index)}(vm);
"""
        )
    out.write("""    default:
        VERIFY_NOT_REACHED();
    }
}

""")


# https://webidl.spec.whatwg.org/#compute-the-effective-overload-set
def compute_the_effective_overload_set(
    operations: Sequence[Union[Constructor, Operation]],
) -> list[EffectiveOverloadItem]:
    # 1. Let S be an ordered set.
    overloads: list[EffectiveOverloadItem] = []

    # 2. Let F be an ordered set with items as follows, according to the kind of effective overload set.
    # NOTE: The caller provides the relevant operation overload set.

    # 3. Let maxarg be the maximum number of arguments the operations, legacy factory functions, or callback functions
    #    in F are declared to take. For variadic operations and legacy factory functions, the argument on which the
    #    ellipsis appears counts as a single argument.
    maximum_arguments = max(len(operation.parameters) for operation in operations)

    # 4. Let max be max(maxarg, N).
    # NOTE: N is a runtime value. The generated arbiter handles this by switching on min(maxarg, argument_count).

    # 5. For each operation or extended attribute X in F:
    for overload_id, operation in enumerate(operations):
        # 1. Let arguments be the list of arguments X is declared to take.
        arguments = operation.parameters

        # 2. Let n be the size of arguments.
        argument_count = len(arguments)

        # 3. Let types be a type list.
        types: list[IDLType] = []

        # 4. Let optionalityValues be an optionality list.
        optionality_values: list[Optionality] = []

        overload_is_variadic = False

        # 5. For each argument in arguments:
        for argument in arguments:
            # 1. Append the type of argument to types.
            types.append(argument.type)

            # 2. Append "variadic" to optionalityValues if argument is a final, variadic argument, "optional" if
            #    argument is optional, and "required" otherwise.
            if argument.variadic:
                optionality_values.append(Optionality.Variadic)
                overload_is_variadic = True
            elif argument.optional:
                optionality_values.append(Optionality.Optional)
            else:
                optionality_values.append(Optionality.Required)

        # 6. Append the tuple (X, types, optionalityValues) to S.
        overloads.append(EffectiveOverloadItem(overload_id, types, optionality_values))

        # 7. If X is declared to be variadic, then:
        if overload_is_variadic:
            # 1. For each i in the range n to max - 1, inclusive:
            for i in range(argument_count, maximum_arguments):
                item_types = list(types)
                item_optionality_values = list(optionality_values)

                # 4. For each j in the range n to i, inclusive:
                for _ in range(argument_count, i + 1):
                    # 1. Append types[n - 1] to t.
                    item_types.append(types[argument_count - 1])

                    # 2. Append "variadic" to o.
                    item_optionality_values.append(Optionality.Variadic)

                # 5. Append the tuple (X, t, o) to S.
                overloads.append(EffectiveOverloadItem(overload_id, item_types, item_optionality_values))

        # 8. Let i be n - 1.
        i = argument_count - 1

        # 9. While i >= 0:
        while i >= 0:
            # 1. If arguments[i] is not optional, then break.
            if not arguments[i].optional and not arguments[i].variadic:
                break

            # 5. Append the tuple (X, t, o) to S.
            overloads.append(EffectiveOverloadItem(overload_id, types[:i], optionality_values[:i]))

            # 6. Set i to i - 1.
            i -= 1

    return overloads


# https://webidl.spec.whatwg.org/#dfn-distinguishing-argument-index
def resolve_distinguishing_argument_index(
    interface: Interface,
    items: list[EffectiveOverloadItem],
    argument_count: int,
    context: GenerationContext,
) -> int:
    for argument_index in range(argument_count):
        found_indistinguishable = False

        for first_item_index, first_item in enumerate(items):
            for second_item in items[first_item_index + 1 :]:
                if not is_distinguishable_from(
                    first_item.types[argument_index],
                    second_item.types[argument_index],
                    interface,
                    context,
                ):
                    found_indistinguishable = True
                    break
            if found_indistinguishable:
                break

        if not found_indistinguishable:
            return argument_index

    raise RuntimeError(f"Could not resolve distinguishing argument index for overloads of '{items[0].callable_id}'")


def constructor_for_idl_type(idl_type: IDLType, context: GenerationContext) -> str:
    nullable = "true" if idl_type.nullable else "false"
    if isinstance(idl_type, IDLParameterizedType):
        parameters = ", ".join(constructor_for_idl_type(parameter, context) for parameter in idl_type.parameters)
        return (
            f'make_ref_counted<WebIDL::ParameterizedType>("{idl_type.name}", {nullable}, '
            f"Vector<NonnullRefPtr<WebIDL::Type const>> {{ {parameters} }})"
        )
    if isinstance(idl_type, IDLUnionType):
        member_types = ", ".join(
            constructor_for_idl_type(member_type, context) for member_type in idl_type.member_types
        )
        return (
            f'make_ref_counted<WebIDL::UnionType>("{idl_type.name}", {nullable}, '
            f"Vector<NonnullRefPtr<WebIDL::Type const>> {{ {member_types} }})"
        )
    return f'make_ref_counted<WebIDL::Type>("{idl_type.name}", {nullable})'


# https://webidl.spec.whatwg.org/#dfn-distinguishable
def is_distinguishable_from(
    left: IDLType,
    right: IDLType,
    interface: Interface,
    context: GenerationContext,
) -> bool:
    # 1. If one type includes a nullable type and the other type either includes a nullable type,
    #    is a union type with flattened member types including a dictionary type, or is a dictionary type,
    #    return false.
    if left.includes_nullable_type() and (
        right.includes_nullable_type() or any(context.dictionary(member) for member in right.flattened_member_types())
    ):
        return False

    # 2. If both types are either a union type or nullable union type, return true if each member type of the one
    #    is distinguishable with each member type of the other, or false otherwise.
    if isinstance(left, IDLUnionType) and isinstance(right, IDLUnionType):
        return all(
            is_distinguishable_from(left_member, right_member, interface, context)
            for left_member in left.member_types
            for right_member in right.member_types
        )

    # 3. If one type is a union type or nullable union type, return true if each member type of the union type is
    #    distinguishable with the non-union type, or false otherwise.
    if isinstance(left, IDLUnionType) or isinstance(right, IDLUnionType):
        if isinstance(left, IDLUnionType):
            the_union = left
            non_union = right
        else:
            assert isinstance(right, IDLUnionType)
            the_union = right
            non_union = left
        return all(
            is_distinguishable_from(non_union, member_type, interface, context)
            for member_type in the_union.member_types
        )

    left_category = distinguishability_category(left, context)
    right_category = distinguishability_category(right, context)

    if left_category == "InterfaceLike" and right_category == "InterfaceLike":
        # The two identified interface-like types are not the same, and
        # FIXME: no single platform object implements both interface-like types.
        return left.name != right.name

    table = {
        "Undefined": {
            "Boolean",
            "Numeric",
            "BigInt",
            "String",
            "Object",
            "Symbol",
            "InterfaceLike",
            "CallbackFunction",
            "SequenceLike",
        },
        "Boolean": {
            "Undefined",
            "Numeric",
            "BigInt",
            "String",
            "Object",
            "Symbol",
            "InterfaceLike",
            "CallbackFunction",
            "DictionaryLike",
            "SequenceLike",
        },
        "Numeric": {
            "Undefined",
            "Boolean",
            "BigInt",
            "String",
            "Object",
            "Symbol",
            "InterfaceLike",
            "CallbackFunction",
            "DictionaryLike",
            "SequenceLike",
        },
        "BigInt": {
            "Undefined",
            "Boolean",
            "Numeric",
            "String",
            "Object",
            "Symbol",
            "InterfaceLike",
            "CallbackFunction",
            "DictionaryLike",
            "SequenceLike",
        },
        "String": {
            "Undefined",
            "Boolean",
            "Numeric",
            "BigInt",
            "Object",
            "Symbol",
            "InterfaceLike",
            "CallbackFunction",
            "DictionaryLike",
            "SequenceLike",
        },
        "Object": {
            "Undefined",
            "Boolean",
            "Numeric",
            "BigInt",
            "String",
            "Symbol",
        },
        "Symbol": {
            "Undefined",
            "Boolean",
            "Numeric",
            "BigInt",
            "String",
            "Object",
            "InterfaceLike",
            "CallbackFunction",
            "DictionaryLike",
            "SequenceLike",
        },
        "InterfaceLike": {
            "Undefined",
            "Boolean",
            "Numeric",
            "BigInt",
            "String",
            "Symbol",
            "CallbackFunction",
            "DictionaryLike",
            "SequenceLike",
        },
        "CallbackFunction": {
            "Undefined",
            "Boolean",
            "Numeric",
            "BigInt",
            "String",
            "Symbol",
            "InterfaceLike",
            "SequenceLike",
        },
        "DictionaryLike": {
            "Boolean",
            "Numeric",
            "BigInt",
            "String",
            "Symbol",
            "InterfaceLike",
            "SequenceLike",
        },
        "SequenceLike": {
            "Undefined",
            "Boolean",
            "Numeric",
            "BigInt",
            "String",
            "Symbol",
            "InterfaceLike",
            "CallbackFunction",
            "DictionaryLike",
        },
    }

    return right_category in table[left_category]


def distinguishability_category(idl_type: IDLType, context: GenerationContext) -> str:
    if idl_type.name == "undefined":
        return "Undefined"
    if idl_type.name == "boolean":
        return "Boolean"
    if is_numeric_type(idl_type.name):
        return "Numeric"
    if idl_type.name == "bigint":
        return "BigInt"
    if is_string_type(idl_type.name):
        return "String"
    if idl_type.name == "object":
        return "Object"
    if idl_type.name == "symbol":
        return "Symbol"
    if context.callback_function(idl_type) is not None:
        return "CallbackFunction"
    if context.dictionary(idl_type) is not None or idl_type.name == "record":
        return "DictionaryLike"
    if isinstance(idl_type, IDLParameterizedType) and idl_type.name in ("sequence", "FrozenArray"):
        return "SequenceLike"

    return "InterfaceLike"


def operation_overload_sets(interface: Interface, static: bool = False) -> dict[str, list[Operation]]:
    overload_sets: dict[str, list[Operation]] = {}
    operations = interface.static_operations if static else interface.regular_operations
    for operation in operations:
        if "FIXME" in operation.extended_attributes:
            continue
        overload_sets.setdefault(operation.name, []).append(operation)
    return overload_sets


def operation_overload_set_length(operations: list[Operation]) -> int:
    return min(operation_length(operation) for operation in operations)


def operation_length(operation: Operation) -> int:
    return parameter_list_length(operation.parameters)


def parameter_list_length(parameters: list[OperationParameter]) -> int:
    return sum(1 for parameter in parameters if not parameter.optional and not parameter.variadic)


def operation_callback_names(interface: Interface) -> set[str]:
    callbacks = set()
    for operations in operation_overload_sets(interface).values():
        operation = operations[0]
        callbacks.add(idl_identifier_cpp_name(operation))
        if len(operations) > 1:
            for overload_index, overloaded_operation in enumerate(operations):
                callbacks.add(idl_identifier_cpp_name(overloaded_operation, suffix=overload_index))
    return callbacks
