# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

# Which language's bindings name which definition. An interface is implemented in one language, but a dictionary is
# data that the bindings of both languages can convert, so it gets a representation in each language that names it,
# wherever its IDL defines it.

from dataclasses import dataclass
from typing import Dict
from typing import Iterable
from typing import List
from typing import Set

from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.rust_members import rust_members
from Generators.libweb_bindings.rust_types import cpp_interface
from Generators.libweb_bindings.rust_types import is_implemented_in_rust
from Generators.libweb_bindings.rust_types import is_named_union
from Generators.libweb_bindings.rust_types import runtime_union_type
from Utils.webidl_parser import Dictionary
from Utils.webidl_parser import IDLType
from Utils.webidl_parser import IDLUnionType
from Utils.webidl_parser import Interface
from Utils.webidl_parser import Module


def types_named_by_interface(interface: Interface, with_unimplemented_members: bool = True) -> List[IDLType]:
    """Every type that the members of the interface name. The bindings of the interface convert them, except those of
    the members marked [FIXME], which they only record as not implemented."""

    def is_wanted(extended_attributes: Dict[str, str]) -> bool:
        return with_unimplemented_members or "FIXME" not in extended_attributes

    types = []
    for attribute in interface.regular_attributes + interface.static_attributes:
        if is_wanted(attribute.extended_attributes):
            types.append(attribute.type)
    for operation in interface.regular_operations + interface.static_operations:
        if not is_wanted(operation.extended_attributes):
            continue
        types.append(operation.return_type)
        types.extend(parameter.type for parameter in operation.parameters)
    for constructor in interface.constructors:
        types.extend(parameter.type for parameter in constructor.parameters)
    special_operations = [
        interface.named_property_getter,
        interface.indexed_property_getter,
        interface.named_property_setter,
        interface.indexed_property_setter,
        interface.named_property_deleter,
    ]
    for special_operation in special_operations:
        if special_operation is not None:
            types.append(special_operation.return_type)
            types.extend(parameter.type for parameter in special_operation.parameters)
    if interface.stringifier is not None and interface.stringifier.attribute is not None:
        types.append(interface.stringifier.attribute.type)
    if interface.iterable is not None:
        types.extend([interface.iterable.key_type, interface.iterable.value_type])
    if interface.async_iterable is not None:
        types.extend([interface.async_iterable.key_type, interface.async_iterable.value_type])
        types.extend(parameter.type for parameter in interface.async_iterable.parameters)
    if interface.maplike is not None:
        types.extend([interface.maplike.key_type, interface.maplike.value_type])
    if interface.setlike is not None:
        types.append(interface.setlike.value_type)
    return [idl_type for idl_type in types if idl_type is not None]


@dataclass
class ConversionDirections:
    """The directions in which the Rust bindings convert a definition."""

    from_js: bool = False
    to_js: bool = False


class DefinitionUsage:
    """Raises the errors of rust_members() for the interfaces implemented in Rust, whose members it reads."""

    def __init__(self, context: GenerationContext) -> None:
        self.context = context
        # The dictionaries that the interfaces implemented in Rust name, in the order of their first use.
        self.rust_dictionaries: Dict[str, ConversionDirections] = {}
        # The unions that the interfaces implemented in Rust name and that have an enum of their own, by their type.
        self.rust_unions: Dict[IDLUnionType, ConversionDirections] = {}
        # The enumerations that the interfaces implemented in Rust name.
        self.rust_enumerations: Dict[str, ConversionDirections] = {}
        # The interfaces that LibWeb implements in C++ and that the interfaces implemented in Rust name.
        self.cpp_interfaces_named_by_rust: Dict[str, Interface] = {}
        # The dictionaries that have a C++ representation.
        self.cpp_dictionary_names: Set[str] = set()
        # The interfaces implemented in Rust that the C++ bindings convert objects of, and so need a C++ class for.
        self.rust_interfaces_named_by_cpp: Dict[str, Interface] = {}
        self._collect_rust_dictionaries()
        self._collect_cpp_dictionaries()
        self._collect_rust_interfaces_named_by_cpp()

    def cpp_dictionaries(self, module: Module) -> List[Dictionary]:
        """The dictionaries of the module that the C++ bindings declare and convert."""
        return [dictionary for dictionary in module.dictionaries if dictionary.name in self.cpp_dictionary_names]

    def _collect_rust_dictionaries(self) -> None:
        # NB: An implementation in Rust converts JS values to a dictionary marked [ImplementedIn=Rust] itself, as the
        #     constructor of ReadableStream converts its underlyingSource object to an UnderlyingSource.
        for dictionary in self.context.dictionaries.values():
            if dictionary.extended_attributes.get("ImplementedIn") == "Rust":
                self._name_for_rust(IDLType(dictionary.name), from_js=True, to_js=False)
        for module in self.context.modules:
            interface = module.interface
            if interface is None or not is_implemented_in_rust(interface):
                continue
            members = rust_members(interface)
            for attribute in members.attributes:
                has_setter = attribute.setter_method is not None
                self._name_for_rust(attribute.attribute.type, from_js=has_setter, to_js=True)
            operations = [
                overload.operation
                for operation_set in members.operation_sets + members.static_operation_sets
                for overload in operation_set.overloads
            ]
            if members.indexed_property_getter is not None:
                operations.append(members.indexed_property_getter.operation.operation)
            for operation in operations:
                self._name_for_rust(operation.return_type, from_js=False, to_js=True)
                for parameter in operation.parameters:
                    self._name_for_rust(parameter.type, from_js=True, to_js=False)
            for constructor in members.constructors:
                for parameter in constructor.constructor.parameters:
                    self._name_for_rust(parameter.type, from_js=True, to_js=False)
            if members.async_iterator is not None:
                for parameter in members.async_iterator.parameters:
                    self._name_for_rust(parameter.type, from_js=True, to_js=False)

    def _name_for_rust(self, idl_type: IDLType, from_js: bool, to_js: bool) -> None:
        if isinstance(idl_type, IDLUnionType):
            # NB: The bindings runtime converts the unions that it has a type for as a whole.
            if runtime_union_type(idl_type, self.context) is not None:
                return
            if is_named_union(idl_type, self.context):
                union_type = idl_type.without_nullable()
                assert isinstance(union_type, IDLUnionType)
                directions = self.rust_unions.setdefault(union_type, ConversionDirections())
                directions.from_js = directions.from_js or from_js
                directions.to_js = directions.to_js or to_js
            for member_type in idl_type.flattened_member_types():
                self._name_for_rust(member_type, from_js, to_js)
            return
        for child_type in idl_type.child_types():
            self._name_for_rust(child_type, from_js, to_js)
        named_cpp_interface = cpp_interface(idl_type, self.context)
        if named_cpp_interface is not None:
            self.cpp_interfaces_named_by_rust[named_cpp_interface.name] = named_cpp_interface
            return
        if idl_type.name in self.context.enumerations:
            directions = self.rust_enumerations.setdefault(idl_type.name, ConversionDirections())
            directions.from_js = directions.from_js or from_js
            directions.to_js = directions.to_js or to_js
            return
        dictionary = self.context.dictionaries.get(idl_type.name)
        if dictionary is None:
            return
        directions = self.rust_dictionaries.setdefault(dictionary.name, ConversionDirections())
        if (directions.from_js or not from_js) and (directions.to_js or not to_js):
            return
        directions.from_js = directions.from_js or from_js
        directions.to_js = directions.to_js or to_js
        for member in self._members_with_inherited(dictionary):
            self._name_for_rust(member.type, from_js, to_js)
        if dictionary.parent_name:
            # NB: The conversion of a dictionary converts the dictionary that it inherits from first.
            self._name_for_rust(IDLType(dictionary.parent_name), from_js, to_js)

    def _members_with_inherited(self, dictionary: Dictionary):
        members = list(dictionary.members)
        parent = self.context.dictionaries.get(dictionary.parent_name) if dictionary.parent_name else None
        if parent is not None:
            members.extend(self._members_with_inherited(parent))
        return members

    def _collect_cpp_dictionaries(self) -> None:
        for module in self.context.modules:
            if is_implemented_in_rust(module.interface):
                continue
            # NB: The C++ bindings declare and convert every dictionary of the modules that they generate, whether
            #     or not an interface names it, unless only the Rust bindings convert JS values to it.
            for dictionary in module.dictionaries:
                directions = self.rust_dictionaries.get(dictionary.name)
                if directions is None or not directions.from_js:
                    self._name_for_cpp(dictionary.name)
            self._name_types_for_cpp(self.types_named_by_cpp_module(module))

    def _collect_rust_interfaces_named_by_cpp(self) -> None:
        def name(types: Iterable[IDLType]) -> None:
            for idl_type in types:
                for nested_type in idl_type.nested_types():
                    interface = self.context.interfaces.get(nested_type.name)
                    if is_implemented_in_rust(interface):
                        assert interface is not None
                        self.rust_interfaces_named_by_cpp[interface.name] = interface

        for module in self.context.modules:
            if not is_implemented_in_rust(module.interface):
                name(self.types_named_by_cpp_module(module, with_unimplemented_members=False))
            for dictionary in self.cpp_dictionaries(module):
                name(member.type for member in dictionary.members)

    def types_named_by_cpp_module(self, module: Module, with_unimplemented_members: bool = True) -> List[IDLType]:
        """The types that the interface, typedefs and callback functions of a module of the C++ bindings name."""
        types = []
        if module.interface is not None:
            types.extend(types_named_by_interface(module.interface, with_unimplemented_members))
        types.extend(typedef.type for typedef in module.typedefs)
        for callback_function in module.callback_functions:
            types.append(callback_function.return_type)
            types.extend(parameter.type for parameter in callback_function.parameters)
        return types

    def _name_types_for_cpp(self, types: Iterable[IDLType]) -> None:
        for idl_type in types:
            for nested_type in idl_type.nested_types():
                if nested_type.name in self.context.dictionaries:
                    self._name_for_cpp(nested_type.name)

    def _name_for_cpp(self, name: str) -> None:
        if name in self.cpp_dictionary_names:
            return
        self.cpp_dictionary_names.add(name)
        dictionary = self.context.dictionaries[name]
        if dictionary.parent_name in self.context.dictionaries:
            self._name_for_cpp(dictionary.parent_name)
        self._name_types_for_cpp(member.type for member in dictionary.members)
