# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from dataclasses import dataclass
from dataclasses import field
from dataclasses import fields
from dataclasses import is_dataclass
from dataclasses import replace
from typing import Any
from typing import Callable
from typing import Iterable
from typing import Optional
from typing import Protocol
from typing import TypeVar
from typing import Union
from typing import cast

from Utils.webidl_parser import CallbackFunction
from Utils.webidl_parser import Dictionary
from Utils.webidl_parser import Enumeration
from Utils.webidl_parser import IDLParameterizedType
from Utils.webidl_parser import IDLType
from Utils.webidl_parser import IDLUnionType
from Utils.webidl_parser import IncludedMixin
from Utils.webidl_parser import Interface
from Utils.webidl_parser import Module
from Utils.webidl_parser import Typedef


class NamedDefinition(Protocol):
    name: str


Definition = TypeVar("Definition", bound=NamedDefinition)


def collect_definitions_by_name(
    modules: list[Module], definitions_for_module: Callable[[Module], Iterable[Definition]]
) -> dict[str, Definition]:
    return {definition.name: definition for module in modules for definition in definitions_for_module(module)}


def collect_partial_definitions_by_name(
    modules: list[Module], definitions_for_module: Callable[[Module], Iterable[Definition]]
) -> dict[str, list[Definition]]:
    partial_definitions: dict[str, list[Definition]] = {}
    for module in modules:
        for definition in definitions_for_module(module):
            partial_definitions.setdefault(definition.name, []).append(definition)
    return partial_definitions


def merge_partial_definitions_by_name(
    modules: list[Module],
    definitions_for_module: Callable[[Module], Iterable[Definition]],
    partial_definitions_for_module: Callable[[Module], Iterable[Definition]],
    merge: Callable[[Definition, list[Definition]], Definition],
) -> dict[str, Definition]:
    definitions = collect_definitions_by_name(modules, definitions_for_module)
    partial_definitions = collect_partial_definitions_by_name(modules, partial_definitions_for_module)
    return {type_: merge(definition, partial_definitions.get(type_, [])) for type_, definition in definitions.items()}


@dataclass
class GenerationContext:
    modules: list[Module]
    callback_functions: dict[str, CallbackFunction] = field(init=False)
    dictionaries: dict[str, Dictionary] = field(init=False)
    enumerations: dict[str, Enumeration] = field(init=False)
    interfaces: dict[str, Interface] = field(init=False)
    typedefs: dict[str, Typedef] = field(init=False)
    mixins: dict[str, Interface] = field(init=False)

    def __post_init__(self) -> None:
        self.callback_functions = collect_definitions_by_name(self.modules, lambda module: module.callback_functions)
        self.enumerations = collect_definitions_by_name(self.modules, lambda module: module.enumerations)
        self.typedefs = collect_definitions_by_name(self.modules, lambda module: module.typedefs)

        self.resolve_dictionaries()
        self.resolve_mixins()
        self.resolve_interfaces()
        self.resolve_typedefs()
        self.resolve_modules()

    def resolve_dictionaries(self) -> None:
        self.dictionaries = merge_partial_definitions_by_name(
            self.modules,
            lambda module: module.dictionaries,
            lambda module: module.partial_dictionaries,
            merge_dictionary,
        )

    def resolve_mixins(self) -> None:
        self.mixins = merge_partial_definitions_by_name(
            self.modules,
            lambda module: module.mixins,
            lambda module: module.partial_mixins,
            merge_mixin,
        )

    def resolve_interfaces(self) -> None:
        interfaces = {
            module.interface.name: module.interface for module in self.modules if module.interface is not None
        }
        partial_interfaces = collect_partial_definitions_by_name(self.modules, lambda module: module.partial_interfaces)
        included_mixins: dict[str, list[IncludedMixin]] = {}
        seen_included_mixins: set[tuple[str, str]] = set()
        for module in self.modules:
            for included_mixin in module.included_mixins:
                key = (
                    included_mixin.interface_name,
                    included_mixin.mixin_name,
                )
                if key in seen_included_mixins:
                    continue
                seen_included_mixins.add(key)
                included_mixins.setdefault(key[0], []).append(included_mixin)
        self.interfaces = {
            type_: merge_interface(
                interface,
                partial_interfaces.get(type_, []),
                included_mixins.get(type_, []),
                self.mixins,
            )
            for type_, interface in interfaces.items()
        }

    def resolve_modules(self) -> None:
        self.modules = [self.resolve_module(module) for module in self.modules]

    def resolve_module(self, module: Module) -> Module:
        return replace(
            module,
            interface=self.interfaces[module.interface.name] if module.interface is not None else None,
            dictionaries=[self.dictionaries[dictionary.name] for dictionary in module.dictionaries],
            mixins=[self.mixins[mixin.name] for mixin in module.mixins],
            partial_interfaces=[],
            partial_dictionaries=[],
            partial_mixins=[],
            included_mixins=[],
        )

    def resolve_typedefs(self) -> None:
        self.typedefs = {
            type_: replace(typedef, type=self.resolve_typedef(typedef.type)) for type_, typedef in self.typedefs.items()
        }
        self.callback_functions = self.resolve_typedefs_in_mapping(self.callback_functions)
        self.dictionaries = self.resolve_typedefs_in_mapping(self.dictionaries)
        self.mixins = self.resolve_typedefs_in_mapping(self.mixins)
        self.interfaces = self.resolve_typedefs_in_mapping(self.interfaces)

    def resolve_typedefs_in_mapping(self, definitions):
        return {type_: self.resolve_typedefs_in(definition) for type_, definition in definitions.items()}

    def resolve_typedefs_in(self, value: Any) -> Any:
        if isinstance(value, IDLType):
            return self.resolve_typedef(value)

        if isinstance(value, list):
            return [self.resolve_typedefs_in(item) for item in value]

        if isinstance(value, tuple):
            return tuple(self.resolve_typedefs_in(item) for item in value)

        if is_dataclass(value):
            return replace(
                cast(Any, value),
                **{
                    field.name: self.resolve_typedefs_in(getattr(value, field.name))
                    for field in fields(value)
                    if field.init
                },
            )

        return value

    def callback_function(self, type_: IDLType) -> Optional[CallbackFunction]:
        return self.callback_functions.get(type_.name)

    def dictionary(self, type_: IDLType) -> Optional[Dictionary]:
        return self.dictionaries.get(type_.name)

    def dictionary_type_names(self, *types: IDLType) -> set[str]:
        return {
            nested_type.name
            for type_ in types
            for nested_type in type_.nested_types()
            if self.dictionary(nested_type) is not None
        }

    def dictionary_parent(self, dictionary: Dictionary) -> Optional[Dictionary]:
        if not dictionary.parent_name:
            return None

        parent_dictionary = self.dictionaries.get(dictionary.parent_name)
        if parent_dictionary is None:
            raise RuntimeError(
                f"Dictionary '{dictionary.name}' inherits from unknown dictionary '{dictionary.parent_name}'"
            )

        return parent_dictionary

    def dictionary_inheritance_stack(self, dictionary: Dictionary) -> list[Dictionary]:
        stack = [dictionary]

        while stack[-1].parent_name:
            parent = self.dictionary_parent(stack[-1])
            if parent is None:
                break
            stack.append(parent)

        return stack

    # https://webidl.spec.whatwg.org/#create-an-inheritance-stack
    def inheritance_stack(self, interface: Interface) -> list[Interface]:
        # 1. Let stack be a new stack.
        # 2. Push I onto stack.
        stack = [interface]

        # 3. While I inherits from an interface,
        #     1. Let I be that interface.
        #     2. Push I onto stack.
        while stack[-1].parent_name:
            parent = self.interfaces.get(stack[-1].parent_name)
            if parent is None:
                break
            stack.append(parent)

        # 4. Return stack.
        return stack

    def enumeration(self, type_: IDLType) -> Optional[Enumeration]:
        return self.enumerations.get(type_.name)

    def interface(self, type_: IDLType) -> Optional[Interface]:
        return self.interfaces.get(type_.name)

    def resolve_typedef(self, type_: IDLType) -> IDLType:
        resolved_type = type_

        if isinstance(resolved_type, IDLUnionType):
            return IDLUnionType(
                [self.resolve_typedef(member_type) for member_type in resolved_type.member_types],
                resolved_type.nullable,
            )

        if isinstance(resolved_type, IDLParameterizedType):
            return IDLParameterizedType(
                resolved_type.name,
                [self.resolve_typedef(parameter) for parameter in resolved_type.parameters],
                resolved_type.nullable,
            )

        seen_types: set[IDLType] = set()
        while resolved_type.name in self.typedefs:
            resolved_type_without_nullable = resolved_type.without_nullable()
            if resolved_type_without_nullable in seen_types:
                raise RuntimeError(f"Typedef '{resolved_type.name}' resolves recursively")
            seen_types.add(resolved_type_without_nullable)

            typedef_type = self.typedefs[resolved_type.name].type
            resolved_type = typedef_type.clone_with_nullable(resolved_type.nullable or typedef_type.nullable)

            if isinstance(resolved_type, IDLUnionType):
                return IDLUnionType(
                    [self.resolve_typedef(member_type) for member_type in resolved_type.member_types],
                    resolved_type.nullable,
                )

            if isinstance(resolved_type, IDLParameterizedType):
                return IDLParameterizedType(
                    resolved_type.name,
                    [self.resolve_typedef(parameter) for parameter in resolved_type.parameters],
                    resolved_type.nullable,
                )

        return resolved_type


def merge_interface_members(target: Interface, source: Interface) -> None:
    target.constants.extend(source.constants)
    regular_attributes = merge_definition_extended_attributes(source, source.regular_attributes)
    target.regular_attributes.extend(regular_attributes)
    target.static_attributes.extend(merge_definition_extended_attributes(source, source.static_attributes))
    target.regular_operations.extend(merge_definition_extended_attributes(source, source.regular_operations))
    target.static_operations.extend(merge_definition_extended_attributes(source, source.static_operations))
    target.constructors.extend(merge_definition_extended_attributes(source, source.constructors))
    if target.stringifier is None and source.stringifier is not None:
        if source.stringifier.attribute is None:
            target.stringifier = source.stringifier
        else:
            attribute_index = source.regular_attributes.index(source.stringifier.attribute)
            target.stringifier = replace(
                source.stringifier,
                extended_attributes=regular_attributes[attribute_index].extended_attributes,
                attribute=regular_attributes[attribute_index],
            )
    target.named_property_getter = target.named_property_getter or source.named_property_getter
    target.indexed_property_getter = target.indexed_property_getter or source.indexed_property_getter
    target.named_property_setter = target.named_property_setter or source.named_property_setter
    target.named_property_deleter = target.named_property_deleter or source.named_property_deleter
    target.indexed_property_setter = target.indexed_property_setter or source.indexed_property_setter
    target.maplike = target.maplike or source.maplike
    target.setlike = target.setlike or source.setlike
    target.iterable = target.iterable or source.iterable


def merge_interface(
    interface: Interface,
    partial_interfaces: list[Interface],
    included_mixins: list[IncludedMixin],
    mixins: dict[str, Interface],
) -> Interface:
    if not partial_interfaces and not included_mixins:
        return interface

    merged_interface = copy_interface(interface)
    for partial_interface in partial_interfaces:
        if partial_interface.extended_attributes.get("Exposed") == "Nobody":
            continue
        merge_interface_members(merged_interface, partial_interface)

    for included_mixin in included_mixins:
        mixin = mixins.get(included_mixin.mixin_name)
        if mixin is None:
            raise RuntimeError(f"Included mixin '{included_mixin.mixin_name}' does not exist")
        merge_interface_members(merged_interface, mixin)
    return merged_interface


def merge_mixin(mixin: Interface, partial_mixins: list[Interface]) -> Interface:
    if not partial_mixins:
        return mixin

    merged_mixin = copy_interface(mixin)
    for partial_mixin in partial_mixins:
        if partial_mixin.extended_attributes.get("Exposed") == "Nobody":
            continue
        merge_interface_members(merged_mixin, partial_mixin)
    return merged_mixin


def copy_interface(interface: Interface) -> Interface:
    return replace(
        interface,
        constants=list(interface.constants),
        regular_attributes=list(interface.regular_attributes),
        static_attributes=list(interface.static_attributes),
        regular_operations=list(interface.regular_operations),
        static_operations=list(interface.static_operations),
        constructors=list(interface.constructors),
    )


def merge_dictionary(dictionary: Dictionary, partial_dictionaries: list[Dictionary]) -> Dictionary:
    if not partial_dictionaries:
        return dictionary

    merged_dictionary = replace(
        dictionary,
        members=list(dictionary.members),
        extended_attributes=dict(dictionary.extended_attributes),
    )
    for partial_dictionary in partial_dictionaries:
        merged_dictionary.members.extend(
            merge_definition_extended_attributes(partial_dictionary, partial_dictionary.members)
        )
        merged_dictionary.members.sort(key=lambda member: member.name)
    return merged_dictionary


def merge_definition_extended_attributes(source: Union[Interface, Dictionary], members):
    if not source.extended_attributes:
        return members

    return [
        replace(member, extended_attributes={**source.extended_attributes, **member.extended_attributes})
        for member in members
    ]
