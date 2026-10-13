# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

# The members of an interface implemented in Rust, as the Rust bindings handle them: which attributes and operations
# the implementation has methods for and what those methods are called. Everything that asks about the members of such
# an interface asks here, so that what the checks accept, what the definitions are collected from and what the writer
# emits cannot differ. A member that the Rust bindings do not handle is an error that names it.

import re

from dataclasses import dataclass
from dataclasses import field
from typing import List

from Generators.libweb_bindings.overload_resolution import operation_length
from Generators.libweb_bindings.overload_resolution import operation_overload_sets
from Generators.libweb_bindings.overload_resolution import parameter_list_length
from Generators.libweb_bindings.rust_names import rust_identifier
from Generators.libweb_bindings.rust_types import UnsupportedByRustBindings
from Utils.webidl_parser import Attribute
from Utils.webidl_parser import Constructor
from Utils.webidl_parser import Interface
from Utils.webidl_parser import Operation

RUST_METHOD_NAME = re.compile(r"[a-z][a-z0-9_]*")

# The extended attributes of the members of an interface that the Rust bindings handle.
SUPPORTED_ATTRIBUTE_EXTENDED_ATTRIBUTES = {"NewObject", "SameObject"}
SUPPORTED_OPERATION_EXTENDED_ATTRIBUTES = {"NewObject", "SameObject"}


@dataclass
class RustAttribute:
    attribute: Attribute
    # The method of the implementation that the getter calls.
    getter_method: str

    @property
    def getter_function(self) -> str:
        """The generated native function of the getter."""
        return f"{self.getter_method.removeprefix('r#')}_getter"


@dataclass
class RustOperation:
    operation: Operation
    # The method of the implementation that the steps of the operation call.
    method: str

    @property
    def length(self) -> int:
        return operation_length(self.operation)


@dataclass
class RustOperationSet:
    """The operations of an interface that share an identifier, which are one function of the prototype."""

    name: str
    overloads: List[RustOperation]

    @property
    def function(self) -> str:
        """The generated native function that the prototype holds."""
        return f"{rust_identifier(self.name).removeprefix('r#')}_function"

    @property
    def length(self) -> int:
        return min(overload.length for overload in self.overloads)


@dataclass
class RustConstructor:
    constructor: Constructor
    # The method of the implementation that the steps of the constructor call. The constructors of an overloaded set
    # name theirs in the IDL.
    method: str
    # The generated function of the steps of the constructor. The function `construct` is what the entry point of a
    # construction calls: the steps of the only constructor, or the choice between the overloads.
    steps_function: str

    @property
    def length(self) -> int:
        return parameter_list_length(self.constructor.parameters)


@dataclass
class RustMembers:
    attributes: List[RustAttribute] = field(default_factory=list)
    operation_sets: List[RustOperationSet] = field(default_factory=list)
    constructors: List[RustConstructor] = field(default_factory=list)

    @property
    def constructor_length(self) -> int:
        if not self.constructors:
            return 0
        return min(constructor.length for constructor in self.constructors)

    def property_names(self) -> List[str]:
        """The identifiers of the members that the interface prototype object has a property for."""
        names = [attribute.attribute.name for attribute in self.attributes]
        names.extend(operation_set.name for operation_set in self.operation_sets)
        return names


def rust_members(interface: Interface) -> RustMembers:
    """Raises an error that names the member for every member that the Rust bindings do not handle yet."""

    def fail(message: str) -> None:
        raise UnsupportedByRustBindings(f"{interface.path}: {interface.name} is [ImplementedIn=Rust], but {message}")

    def check_extended_attributes(member_name: str, extended_attributes: dict, supported: set) -> None:
        unsupported = set(extended_attributes) - supported
        if unsupported:
            fail(f"the Rust bindings do not handle the extended attributes {sorted(unsupported)} of {member_name}")

    special_operations = [
        interface.named_property_getter,
        interface.indexed_property_getter,
        interface.named_property_setter,
        interface.indexed_property_setter,
        interface.named_property_deleter,
    ]
    if any(special_operations) or interface.stringifier is not None:
        fail("it has special operations")
    if interface.iterable or interface.async_iterable or interface.maplike or interface.setlike:
        fail("it is iterable or a maplike or setlike")
    if interface.static_attributes or interface.static_operations or interface.constants:
        fail("it has static members or constants")

    members = RustMembers()

    constructor_attributes = {"ImplementedAs"} if len(interface.constructors) > 1 else set()
    for constructor in interface.constructors:
        unsupported_attributes = set(constructor.extended_attributes) - constructor_attributes
        if unsupported_attributes:
            fail(
                "the Rust bindings do not handle the extended attributes "
                f"{sorted(unsupported_attributes)} of its constructor"
            )
    if len(interface.constructors) > 1:
        # NB: Rust has no overloading, and a name made up from the position of a constructor in the IDL would swap
        #     two overloads silently when the IDL is reordered, so the IDL names the method of every overload.
        methods = [constructor.extended_attributes.get("ImplementedAs") for constructor in interface.constructors]
        if not all(method is not None and RUST_METHOD_NAME.fullmatch(method) for method in methods):
            fail("its constructors are overloaded, and not all of them name their Rust method with [ImplementedAs]")
        if len(set(methods)) != len(methods):
            fail(f"its constructors name the same Rust method with [ImplementedAs] more than once: {methods}")
        for constructor in interface.constructors:
            method = constructor.extended_attributes["ImplementedAs"]
            members.constructors.append(RustConstructor(constructor, method, f"{method}_steps"))
    else:
        for constructor in interface.constructors:
            members.constructors.append(RustConstructor(constructor, "construct", "construct"))

    # NB: operation_overload_sets() leaves out the operations marked [FIXME], which C++ defines as unimplemented.
    for operation in interface.regular_operations:
        check_extended_attributes(
            operation.name, operation.extended_attributes, SUPPORTED_OPERATION_EXTENDED_ATTRIBUTES
        )

    for attribute in interface.regular_attributes:
        check_extended_attributes(
            attribute.name, attribute.extended_attributes, SUPPORTED_ATTRIBUTE_EXTENDED_ATTRIBUTES
        )
        if not attribute.readonly:
            fail(f"its attribute {attribute.name} is not readonly")
        members.attributes.append(RustAttribute(attribute, rust_identifier(attribute.name)))

    for name, operations in operation_overload_sets(interface).items():
        if len(operations) > 1:
            fail(f"its operation {name} is overloaded")
        overloads = [RustOperation(operation, rust_identifier(name)) for operation in operations]
        members.operation_sets.append(RustOperationSet(name, overloads))

    return members
