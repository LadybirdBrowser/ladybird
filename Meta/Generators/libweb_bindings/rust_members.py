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
from typing import Optional
from typing import Union

from Generators.libweb_bindings.overload_resolution import operation_length
from Generators.libweb_bindings.overload_resolution import operation_overload_sets
from Generators.libweb_bindings.overload_resolution import parameter_list_length
from Generators.libweb_bindings.rust_names import rust_identifier
from Generators.libweb_bindings.rust_types import INTEGER_TYPES
from Generators.libweb_bindings.rust_types import UnsupportedByRustBindings
from Generators.libweb_bindings.rust_types import idl_integer_value
from Utils.webidl_parser import Attribute
from Utils.webidl_parser import Constructor
from Utils.webidl_parser import Interface
from Utils.webidl_parser import Operation

RUST_METHOD_NAME = re.compile(r"[a-z][a-z0-9_]*")

# The extended attributes of the members of an interface that the Rust bindings handle.
SUPPORTED_ATTRIBUTE_EXTENDED_ATTRIBUTES = {"FIXME", "ImplementedAs", "NewObject", "SameObject"}
SUPPORTED_OPERATION_EXTENDED_ATTRIBUTES = {"FIXME", "ImplementedAs", "NewObject", "SameObject"}


def generated_function_stem(method: str) -> str:
    """What the names of the generated functions of a member start with: the method without what makes a reserved
    name usable as an identifier, which a longer name does not need."""
    return method.removeprefix("r#").removesuffix("_")


@dataclass
class RustAttribute:
    attribute: Attribute
    # The method of the implementation that the getter calls.
    getter_method: str
    # The method of the implementation that the setter calls, for an attribute that is not read only.
    setter_method: Optional[str]

    @property
    def getter_function(self) -> str:
        """The generated native function of the getter."""
        return f"{generated_function_stem(self.getter_method)}_getter"

    @property
    def setter_function(self) -> str:
        """The generated native function of the setter."""
        return f"{generated_function_stem(self.getter_method)}_setter"


@dataclass
class RustOperation:
    operation: Operation
    # The method of the implementation that the steps of the operation call.
    method: str

    @property
    def length(self) -> int:
        return operation_length(self.operation)

    @property
    def steps_function(self) -> str:
        """The generated function of the steps of the operation, when the operation is one of several overloads that
        the function of the prototype chooses between."""
        return f"{generated_function_stem(self.method)}_steps"


@dataclass
class RustOperationSet:
    """The operations of an interface that share an identifier, which are one function of the prototype."""

    name: str
    overloads: List[RustOperation]

    @property
    def function(self) -> str:
        """The generated native function that the prototype holds."""
        return f"{generated_function_stem(rust_identifier(self.name))}_function"

    @property
    def length(self) -> int:
        return min(overload.length for overload in self.overloads)


@dataclass
class RustConstant:
    name: str
    value: int


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
    # The attributes and the operations that the implementation has methods for.
    attributes: List[RustAttribute] = field(default_factory=list)
    operation_sets: List[RustOperationSet] = field(default_factory=list)
    constructors: List[RustConstructor] = field(default_factory=list)
    constants: List[RustConstant] = field(default_factory=list)
    # The regular attributes in the order of the IDL, as the prototype defines them: an attribute of `attributes`, or
    # the identifier of one marked [FIXME].
    attributes_in_order: List[Union[RustAttribute, str]] = field(default_factory=list)
    # The identifiers of the operations marked [FIXME] that have no implemented overload, each once.
    unimplemented_operation_names: List[str] = field(default_factory=list)

    @property
    def constructor_length(self) -> int:
        if not self.constructors:
            return 0
        return min(constructor.length for constructor in self.constructors)

    def property_names(self) -> List[str]:
        """The identifiers of the members that the interface prototype object has a property for, in the order of
        their definition."""
        names = [attribute.attribute.name for attribute in self.attributes]
        names.extend(operation_set.name for operation_set in self.operation_sets)
        names.extend(constant.name for constant in self.constants)
        return names

    def unimplemented_property_names(self) -> List[str]:
        """The identifiers of the members that the interface prototype object records as not implemented."""
        names = [attribute for attribute in self.attributes_in_order if isinstance(attribute, str)]
        names.extend(self.unimplemented_operation_names)
        return names


def rust_members(interface: Interface) -> RustMembers:
    """Raises an error that names the member for every member that the Rust bindings do not handle yet."""

    def fail(message: str) -> None:
        raise UnsupportedByRustBindings(f"{interface.path}: {interface.name} is [ImplementedIn=Rust], but {message}")

    def check_extended_attributes(member_name: str, extended_attributes: dict, supported: set) -> None:
        unsupported = set(extended_attributes) - supported
        if unsupported:
            fail(f"the Rust bindings do not handle the extended attributes {sorted(unsupported)} of {member_name}")

    def method_of(member_name: str, extended_attributes: dict) -> str:
        """The method of the implementation for a member, which the IDL can name with [ImplementedAs]."""
        method = extended_attributes.get("ImplementedAs")
        if method is None:
            return rust_identifier(member_name)
        if not RUST_METHOD_NAME.fullmatch(method):
            fail(f"[ImplementedAs={method}] of {member_name} does not name a Rust method")
        return method

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
    if interface.static_attributes or interface.static_operations:
        fail("it has static members")

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

    for attribute in interface.regular_attributes:
        check_extended_attributes(
            attribute.name, attribute.extended_attributes, SUPPORTED_ATTRIBUTE_EXTENDED_ATTRIBUTES
        )
        if "FIXME" in attribute.extended_attributes:
            members.attributes_in_order.append(attribute.name)
            continue
        getter_method = method_of(attribute.name, attribute.extended_attributes)
        setter_method = None if attribute.readonly else f"set_{generated_function_stem(getter_method)}"
        rust_attribute = RustAttribute(attribute, getter_method, setter_method)
        members.attributes.append(rust_attribute)
        members.attributes_in_order.append(rust_attribute)

    for operation in interface.regular_operations:
        check_extended_attributes(
            operation.name, operation.extended_attributes, SUPPORTED_OPERATION_EXTENDED_ATTRIBUTES
        )

    # NB: operation_overload_sets() leaves out the operations marked [FIXME], which the prototype records as not
    #     implemented unless another overload of theirs is.
    overload_sets = operation_overload_sets(interface)
    for name, operations in overload_sets.items():
        if len(operations) > 1:
            # NB: As for constructors, the IDL names the Rust method of every overload.
            if not all("ImplementedAs" in operation.extended_attributes for operation in operations):
                fail(
                    f"its operation {name} is overloaded, and not all of the overloads name their Rust method with "
                    "[ImplementedAs]"
                )
            if any(operation.return_type_is_promise() for operation in operations):
                fail(f"its operation {name} is overloaded and returns a promise type")
        overloads = [
            RustOperation(operation, method_of(name, operation.extended_attributes)) for operation in operations
        ]
        members.operation_sets.append(RustOperationSet(name, overloads))
    for operation in interface.regular_operations:
        is_unimplemented = "FIXME" in operation.extended_attributes and operation.name not in overload_sets
        if is_unimplemented and operation.name not in members.unimplemented_operation_names:
            members.unimplemented_operation_names.append(operation.name)

    for constant in interface.constants:
        value = idl_integer_value(constant.value, constant.type.name) if constant.type.name in INTEGER_TYPES else None
        if value is None:
            fail(f"its constant {constant.name} is not an integer of the type {constant.type}")
        else:
            members.constants.append(RustConstant(constant.name, value))

    methods = [attribute.getter_method for attribute in members.attributes]
    methods.extend(attribute.setter_method for attribute in members.attributes if attribute.setter_method is not None)
    methods.extend(overload.method for operation_set in members.operation_sets for overload in operation_set.overloads)
    methods.extend(constructor.method for constructor in members.constructors)
    repeated_methods = sorted({method for method in methods if methods.count(method) > 1})
    if repeated_methods:
        fail(f"more than one of its members has the Rust method {repeated_methods}")

    return members
