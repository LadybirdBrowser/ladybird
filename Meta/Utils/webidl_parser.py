# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from dataclasses import dataclass
from dataclasses import field
from enum import Enum
from pathlib import Path
from typing import Dict
from typing import List
from typing import NoReturn
from typing import Optional
from typing import Sequence
from typing import Tuple

from Utils.lexer import Lexer


@dataclass(frozen=True)
class IDLType:
    name: str
    nullable: bool = False

    def __str__(self) -> str:
        nullable_suffix = "?" if self.nullable else ""
        return f"{self.name}{nullable_suffix}"

    def clone_with_nullable(self, nullable: bool) -> "IDLType":
        return IDLType(self.name, nullable)

    def without_nullable(self) -> "IDLType":
        return self.clone_with_nullable(False)

    def flattened_member_types(self) -> List["IDLType"]:
        return [self]

    def includes_undefined(self) -> bool:
        return self.name == "undefined"

    def includes_nullable_type(self) -> bool:
        return self.nullable

    def child_types(self) -> Tuple["IDLType", ...]:
        return ()

    def nested_types(self) -> List["IDLType"]:
        nested_types: List["IDLType"] = [self]
        for child_type in self.child_types():
            nested_types.extend(child_type.nested_types())
        return nested_types


@dataclass(init=False, frozen=True)
class IDLUnionType(IDLType):
    member_types: Tuple[IDLType, ...]

    def __init__(self, member_types: Sequence[IDLType], nullable: bool = False) -> None:
        object.__setattr__(self, "member_types", tuple(member_types))
        super().__init__(self.name_for_member_types(member_types), nullable)

    @staticmethod
    def name_for_member_types(member_types: Sequence[IDLType]) -> str:
        return f"({' or '.join(str(member_type) for member_type in member_types)})"

    def __str__(self) -> str:
        nullable_suffix = "?" if self.nullable else ""
        return f"{self.name_for_member_types(self.member_types)}{nullable_suffix}"

    def clone_with_nullable(self, nullable: bool) -> "IDLUnionType":
        return IDLUnionType(self.member_types, nullable)

    def flattened_member_types(self) -> List[IDLType]:
        flattened_member_types: List[IDLType] = []
        for member_type in self.member_types:
            flattened_member_types.extend(member_type.flattened_member_types())
        return flattened_member_types

    def includes_undefined(self) -> bool:
        return any(member_type.includes_undefined() for member_type in self.member_types)

    def includes_nullable_type(self) -> bool:
        return self.nullable or any(member_type.includes_nullable_type() for member_type in self.member_types)

    def child_types(self) -> Tuple[IDLType, ...]:
        return self.member_types


@dataclass(init=False, frozen=True)
class IDLParameterizedType(IDLType):
    parameters: Tuple[IDLType, ...]

    def __init__(self, name: str, parameters: Sequence[IDLType], nullable: bool = False) -> None:
        object.__setattr__(self, "parameters", tuple(parameters))
        super().__init__(name, nullable)

    def __str__(self) -> str:
        nullable_suffix = "?" if self.nullable else ""
        return f"{self.name}<{', '.join(str(parameter) for parameter in self.parameters)}>{nullable_suffix}"

    def clone_with_nullable(self, nullable: bool) -> "IDLParameterizedType":
        return IDLParameterizedType(self.name, self.parameters, nullable)

    def child_types(self) -> Tuple[IDLType, ...]:
        return self.parameters


@dataclass
class Constant:
    type: IDLType
    name: str
    value: str


@dataclass
class OperationParameter:
    name: str
    type: IDLType
    optional: bool = False
    default_value: Optional[str] = None
    variadic: bool = False
    extended_attributes: Dict[str, str] = field(default_factory=dict)


@dataclass
class SpecialOperation:
    identifier_type: IDLType
    return_type: IDLType
    name: str
    parameters: List[OperationParameter]
    extended_attributes: Dict[str, str] = field(default_factory=dict)


@dataclass
class Operation:
    name: str
    return_type: IDLType
    parameters: List[OperationParameter]
    extended_attributes: Dict[str, str] = field(default_factory=dict)

    def return_type_is_promise(self) -> bool:
        return self.return_type.name == "Promise"


@dataclass
class Constructor:
    parameters: List[OperationParameter]
    extended_attributes: Dict[str, str] = field(default_factory=dict)


@dataclass
class IterableDeclaration:
    value_type: IDLType
    key_type: Optional[IDLType] = None


@dataclass
class AsyncIterableDeclaration:
    value_type: IDLType
    parameters: List[OperationParameter] = field(default_factory=list)
    key_type: Optional[IDLType] = None


@dataclass
class MaplikeDeclaration:
    key_type: IDLType
    value_type: IDLType
    readonly: bool = False


@dataclass
class SetlikeDeclaration:
    value_type: IDLType
    readonly: bool = False


@dataclass
class Attribute:
    name: str
    type: IDLType
    readonly: bool = False
    inherit: bool = False
    extended_attributes: Dict[str, str] = field(default_factory=dict)


@dataclass
class Stringifier:
    extended_attributes: Dict[str, str] = field(default_factory=dict)
    attribute: Optional[Attribute] = None


class InterfaceKind(Enum):
    Interface = "interface"
    Namespace = "namespace"
    CallbackInterface = "callback interface"


@dataclass
class Interface:
    name: str
    path: Path
    extended_attributes: Dict[str, str] = field(default_factory=dict)
    kind: InterfaceKind = InterfaceKind.Interface
    parent_name: str = ""
    constants: List[Constant] = field(default_factory=list)
    regular_attributes: List[Attribute] = field(default_factory=list)
    static_attributes: List[Attribute] = field(default_factory=list)
    regular_operations: List[Operation] = field(default_factory=list)
    static_operations: List[Operation] = field(default_factory=list)
    constructors: List[Constructor] = field(default_factory=list)
    stringifier: Optional[Stringifier] = None
    iterable: Optional[IterableDeclaration] = None
    async_iterable: Optional[AsyncIterableDeclaration] = None
    named_property_getter: Optional[SpecialOperation] = None
    indexed_property_getter: Optional[SpecialOperation] = None
    named_property_setter: Optional[SpecialOperation] = None
    named_property_deleter: Optional[SpecialOperation] = None
    indexed_property_setter: Optional[SpecialOperation] = None
    maplike: Optional[MaplikeDeclaration] = None
    setlike: Optional[SetlikeDeclaration] = None

    @property
    def is_namespace(self) -> bool:
        return self.kind == InterfaceKind.Namespace

    @property
    def is_callback_interface(self) -> bool:
        return self.kind == InterfaceKind.CallbackInterface

    @property
    def has_special_member(self) -> bool:
        return (
            self.iterable is not None
            or self.async_iterable is not None
            or self.maplike is not None
            or self.setlike is not None
        )

    @property
    def namespaced_name(self) -> str:
        legacy_namespace = self.extended_attributes.get("LegacyNamespace")
        if legacy_namespace:
            return f"{legacy_namespace}.{self.name}"
        return self.name

    @property
    def implemented_name(self) -> str:
        return self.extended_attributes.get("ImplementedAs", self.name)

    @property
    def constructor_class(self) -> str:
        return f"{self.implemented_name}Constructor"

    @property
    def prototype_class(self) -> str:
        return f"{self.implemented_name}Prototype"

    @property
    def namespace_class(self) -> str:
        return f"{self.name}Namespace"

    @property
    def supports_named_properties(self) -> bool:
        return self.named_property_getter is not None


@dataclass
class Dictionary:
    name: str
    path: Path
    parent_name: str = ""
    members: List["DictionaryMember"] = field(default_factory=list)
    extended_attributes: Dict[str, str] = field(default_factory=dict)


@dataclass
class DictionaryMember:
    name: str
    type: IDLType
    required: bool = False
    default_value: Optional[str] = None
    extended_attributes: Dict[str, str] = field(default_factory=dict)


@dataclass
class CallbackFunction:
    name: str
    path: Path
    return_type: IDLType
    parameters: List[OperationParameter] = field(default_factory=list)
    extended_attributes: Dict[str, str] = field(default_factory=dict)


@dataclass
class Enumeration:
    name: str
    path: Path
    values: List[str]
    extended_attributes: Dict[str, str] = field(default_factory=dict)


@dataclass
class Typedef:
    name: str
    path: Path
    type: IDLType


@dataclass
class IncludedMixin:
    interface_name: str
    mixin_name: str


@dataclass
class Module:
    path: Path
    interface: Optional[Interface] = None
    partial_interfaces: List[Interface] = field(default_factory=list)
    mixins: List[Interface] = field(default_factory=list)
    partial_mixins: List[Interface] = field(default_factory=list)
    included_mixins: List[IncludedMixin] = field(default_factory=list)
    callback_functions: List[CallbackFunction] = field(default_factory=list)
    dictionaries: List[Dictionary] = field(default_factory=list)
    partial_dictionaries: List[Dictionary] = field(default_factory=list)
    enumerations: List[Enumeration] = field(default_factory=list)
    typedefs: List[Typedef] = field(default_factory=list)


class ParseError(RuntimeError):
    pass


def parse_module(path: Path, contents: str) -> Module:
    return Parser(path, contents).parse()


class Parser:
    def __init__(self, path: Path, contents: str) -> None:
        self.path = path
        self.contents = contents
        self.lexer = Lexer(contents)

    def parse(self) -> Module:
        module = Module(path=self.path)

        self.consume_whitespace()
        while not self.lexer.is_eof():
            self.parse_module_declaration(module, self.parse_leading_extended_attributes())
            self.consume_whitespace()

        return module

    def parse_module_declaration(self, module: Module, extended_attributes: Dict[str, str]) -> None:
        if self.consume_optional_keyword("dictionary"):
            module.dictionaries.append(self.parse_dictionary(extended_attributes))
            return

        if self.consume_optional_keyword("partial dictionary"):
            module.partial_dictionaries.append(self.parse_dictionary(extended_attributes))
            return

        if self.consume_optional_keyword("enum"):
            module.enumerations.append(self.parse_enumeration(extended_attributes))
            return

        if self.consume_optional_keyword("typedef"):
            module.typedefs.append(self.parse_typedef())
            return

        if self.consume_optional_keyword("partial interface mixin"):
            module.partial_mixins.append(self.parse_interface_declaration(extended_attributes))
            return

        if self.consume_optional_keyword("partial interface"):
            module.partial_interfaces.append(self.parse_interface_declaration(extended_attributes))
            return

        if self.consume_optional_keyword("interface mixin"):
            module.mixins.append(self.parse_interface_declaration(extended_attributes))
            return

        if self.consume_optional_keyword("partial namespace"):
            module.partial_interfaces.append(
                self.parse_interface_declaration(extended_attributes, InterfaceKind.Namespace)
            )
            return

        if self.consume_optional_keyword("callback interface"):
            module.interface = self.parse_interface_declaration(extended_attributes, InterfaceKind.CallbackInterface)
            return

        if self.consume_optional_keyword("callback"):
            module.callback_functions.append(self.parse_callback_function(extended_attributes))
            return

        if self.consume_optional_keyword("namespace"):
            module.interface = self.parse_interface_declaration(extended_attributes, InterfaceKind.Namespace)
            return

        if self.consume_optional_keyword("interface"):
            module.interface = self.parse_interface_declaration(extended_attributes)
            return

        module.included_mixins.append(self.parse_includes_statement())

    def parse_includes_statement(self) -> IncludedMixin:
        interface_name = self.parse_identifier()
        self.consume_whitespace()

        if not self.next_is_keyword("includes"):
            self.raise_parse_error("expected a declaration or an includes statement")

        self.consume_keyword("includes")
        self.consume_whitespace()
        mixin_name = self.parse_identifier()
        self.consume_whitespace()
        self.assert_specific(";")
        return IncludedMixin(interface_name=interface_name, mixin_name=mixin_name)

    def parse_interface_declaration(
        self, extended_attributes: Dict[str, str], kind: InterfaceKind = InterfaceKind.Interface
    ) -> Interface:
        name, parent_name = self.parse_named_body_header()
        interface = Interface(
            name=name,
            path=self.path,
            extended_attributes=extended_attributes,
            kind=kind,
            parent_name=parent_name,
        )
        self.parse_interface_body(interface)
        self.consume_whitespace()
        self.assert_specific(";")

        return interface

    def parse_dictionary(self, extended_attributes: Dict[str, str]) -> Dictionary:
        dictionary_extended_attributes = extended_attributes
        dictionary_name, parent_name = self.parse_named_body_header()
        members: List[DictionaryMember] = []

        while True:
            self.consume_whitespace()

            if self.lexer.consume_specific("}"):
                self.consume_whitespace()
                self.assert_specific(";")
                break

            member_extended_attributes = self.parse_leading_extended_attributes()

            required = self.consume_optional_keyword("required")

            member_extended_attributes.update(self.parse_leading_extended_attributes())

            member_type = self.parse_type()
            self.consume_whitespace()
            member_name = self.parse_identifier()
            self.consume_whitespace()

            default_value: Optional[str] = None
            if self.lexer.consume_specific("="):
                if required:
                    self.raise_parse_error("required dictionary member must not have a default value")
                self.consume_whitespace()
                default_value = self.consume_until_top_level(";").strip()
                self.consume_whitespace()

            self.assert_specific(";")

            members.append(
                DictionaryMember(
                    name=member_name,
                    type=member_type,
                    required=required,
                    default_value=default_value,
                    extended_attributes=member_extended_attributes,
                )
            )

        members.sort(key=lambda member: member.name)

        return Dictionary(
            name=dictionary_name,
            path=self.path,
            parent_name=parent_name,
            members=members,
            extended_attributes=dictionary_extended_attributes,
        )

    def parse_callback_function(self, extended_attributes: Dict[str, str]) -> CallbackFunction:
        name = self.parse_identifier()
        self.consume_whitespace()
        self.assert_specific("=")
        self.consume_whitespace()

        return_type = self.parse_type()
        self.consume_whitespace()
        parameters = self.parse_parenthesized_parameters()
        self.assert_specific(";")

        return CallbackFunction(
            name=name,
            path=self.path,
            return_type=return_type,
            parameters=parameters,
            extended_attributes=extended_attributes,
        )

    def parse_typedef(self) -> Typedef:
        typedef_type = self.parse_type()
        self.consume_whitespace()

        name = self.parse_identifier()
        self.consume_whitespace()
        self.assert_specific(";")

        return Typedef(name=name, path=self.path, type=typedef_type)

    def parse_type(self) -> IDLType:
        if self.lexer.consume_specific("("):
            return self.parse_union_type()

        type_name = self.parse_type_name()
        parameters: List[IDLType] = []
        if self.lexer.next_is("<"):
            parameters = self.parse_type_parameters()

        nullable = self.lexer.consume_specific("?")
        if parameters:
            return IDLParameterizedType(type_name, parameters, nullable)
        return IDLType(type_name, nullable)

    def parse_union_type(self) -> IDLUnionType:
        member_types = [self.parse_type()]
        self.consume_whitespace()
        self.consume_keyword("or")
        self.consume_whitespace()
        member_types.append(self.parse_type())
        self.consume_whitespace()

        while self.lexer.consume_specific("or"):
            self.consume_whitespace()
            member_types.append(self.parse_type())
            self.consume_whitespace()

        self.assert_specific(")")
        return IDLUnionType(member_types, self.lexer.consume_specific("?"))

    def parse_type_name(self) -> str:
        type_words: List[str] = []
        if self.consume_optional_keyword("unsigned"):
            type_words.append("unsigned")
        if self.consume_optional_keyword("unrestricted"):
            type_words.append("unrestricted")

        name = self.lexer.consume_while(lambda character: character.isalnum() or character == "_")
        if not name:
            self.raise_parse_error("type can't be an empty string")
        type_words.append(name)

        if name.lower() == "long":
            position_before_whitespace = self.lexer.tell()
            self.consume_whitespace()
            if self.lexer.consume_specific("long"):
                type_words.append("long")
            else:
                self.lexer.position = position_before_whitespace

        return " ".join(type_words)

    def parse_named_body_header(self) -> Tuple[str, str]:
        name = self.parse_identifier()
        self.consume_whitespace()

        parent_name = ""
        if self.lexer.consume_specific(":"):
            self.consume_whitespace()
            parent_name = self.parse_identifier()
            self.consume_whitespace()

        self.assert_specific("{")
        return name, parent_name

    def parse_enumeration(self, extended_attributes: Dict[str, str]) -> Enumeration:
        name = self.parse_identifier()
        self.consume_whitespace()
        self.assert_specific("{")

        values: List[str] = []

        while not self.lexer.is_eof():
            self.consume_whitespace()
            if self.lexer.consume_specific("}"):
                break

            self.assert_specific('"')
            value = self.lexer.consume_until(lambda character: character == '"')
            self.assert_specific('"')
            self.consume_whitespace()

            values.append(value)

            self.consume_whitespace()
            if self.lexer.next_is("}"):
                continue
            self.assert_specific(",")

        self.consume_whitespace()
        self.assert_specific(";")

        return Enumeration(
            name=name,
            path=self.path,
            values=values,
            extended_attributes=extended_attributes,
        )

    def parse_interface_body(self, interface: Interface) -> None:
        while not self.lexer.is_eof():
            self.consume_whitespace()
            if self.lexer.consume_specific("}"):
                return

            extended_attributes = self.parse_leading_extended_attributes()
            self.consume_whitespace()

            if self.next_is_keyword("const"):
                interface.constants.append(self.parse_constant())
                continue

            if self.consume_optional_keyword("static"):
                readonly = self.consume_optional_keyword("readonly")
                if readonly or self.next_is_keyword("attribute"):
                    interface.static_attributes.append(self.parse_attribute(extended_attributes, readonly=readonly))
                else:
                    interface.static_operations.append(self.parse_operation(extended_attributes))
                continue

            if self.consume_optional_keyword("stringifier"):
                readonly = self.consume_optional_keyword("readonly")
                if readonly or self.next_is_keyword("attribute"):
                    attribute = self.parse_attribute(extended_attributes, readonly=readonly)
                    interface.regular_attributes.append(attribute)
                    interface.stringifier = Stringifier(extended_attributes, attribute)
                    continue

                self.consume_member_semicolon()
                interface.stringifier = Stringifier(extended_attributes)
                continue

            if self.consume_optional_keyword("inherit"):
                interface.regular_attributes.append(self.parse_attribute(extended_attributes, inherit=True))
                continue

            readonly = self.consume_optional_keyword("readonly")
            if self.next_is_keyword("attribute"):
                interface.regular_attributes.append(
                    self.parse_attribute(
                        extended_attributes,
                        readonly=readonly,
                    )
                )
                continue

            if self.next_is_keyword("maplike"):
                interface.maplike = self.parse_maplike_declaration(readonly=readonly)
                continue

            if self.next_is_keyword("setlike"):
                interface.setlike = self.parse_setlike_declaration(readonly=readonly)
                continue

            if readonly:
                interface.regular_attributes.append(self.parse_attribute(extended_attributes, readonly=True))
                continue

            if self.next_is_keyword("constructor"):
                interface.constructors.append(self.parse_constructor(extended_attributes))
                continue

            if self.next_is_keyword("iterable"):
                interface.iterable = self.parse_iterable_declaration()
                continue

            if self.next_is_keyword("async"):
                interface.async_iterable = self.parse_async_iterable_declaration()
                continue

            if self.next_is_keyword("getter"):
                special_operation = self.parse_special_operation("getter", extended_attributes)
                identifier_type = special_operation.identifier_type

                if identifier_type.name == "DOMString" and not identifier_type.nullable:
                    interface.named_property_getter = special_operation
                elif identifier_type.name == "unsigned long" and not identifier_type.nullable:
                    interface.indexed_property_getter = special_operation
                else:
                    self.raise_parse_error(
                        f"named/indexed property getter must use DOMString or unsigned long, got '{identifier_type}'"
                    )
                continue

            if self.next_is_keyword("setter"):
                special_operation = self.parse_special_operation("setter", extended_attributes)
                identifier_type = special_operation.identifier_type

                if identifier_type.name == "DOMString" and not identifier_type.nullable:
                    interface.named_property_setter = special_operation
                elif identifier_type.name == "unsigned long" and not identifier_type.nullable:
                    interface.indexed_property_setter = special_operation
                else:
                    self.raise_parse_error(
                        f"named/indexed property setter must use DOMString or unsigned long, got '{identifier_type}'"
                    )
                continue

            if self.next_is_keyword("deleter"):
                special_operation = self.parse_special_operation("deleter", extended_attributes)
                identifier_type = special_operation.identifier_type

                if identifier_type.name == "DOMString" and not identifier_type.nullable:
                    interface.named_property_deleter = special_operation
                else:
                    self.raise_parse_error(f"named property deleter must use DOMString, got '{identifier_type}'")
                continue

            interface.regular_operations.append(self.parse_operation(extended_attributes))

        self.raise_parse_error("unterminated interface body")

    def parse_iterable_declaration(self) -> IterableDeclaration:
        self.consume_keyword("iterable")
        key_type, value_type = self.parse_iterable_type_parameters()
        self.consume_member_semicolon()

        return IterableDeclaration(
            value_type=value_type,
            key_type=key_type,
        )

    def parse_async_iterable_declaration(self) -> AsyncIterableDeclaration:
        self.consume_keyword("async")
        self.consume_whitespace()
        self.consume_keyword("iterable")
        key_type, value_type = self.parse_iterable_type_parameters()
        self.consume_whitespace()

        parameters: List[OperationParameter] = []
        if self.lexer.next_is("("):
            parameters = self.parse_parenthesized_parameters()

        self.consume_member_semicolon()

        return AsyncIterableDeclaration(
            value_type=value_type,
            key_type=key_type,
            parameters=parameters,
        )

    def parse_iterable_type_parameters(self) -> Tuple[Optional[IDLType], IDLType]:
        parameters = self.parse_type_parameters()
        if len(parameters) == 1:
            return None, parameters[0]
        if len(parameters) == 2:
            return parameters[0], parameters[1]
        self.raise_parse_error("expected one or two iterable type parameters")

    def parse_type_parameters(self) -> List[IDLType]:
        self.assert_specific("<")
        parameters = [self.parse_type()]
        self.consume_whitespace()

        while self.lexer.consume_specific(","):
            self.consume_whitespace()
            parameters.append(self.parse_type())
            self.consume_whitespace()

        self.assert_specific(">")
        return parameters

    def parse_maplike_declaration(self, readonly: bool = False) -> MaplikeDeclaration:
        self.consume_keyword("maplike")
        parameters = self.parse_type_parameters()
        if len(parameters) != 2:
            self.raise_parse_error("expected two maplike type parameters")
        self.consume_member_semicolon()

        return MaplikeDeclaration(
            key_type=parameters[0],
            value_type=parameters[1],
            readonly=readonly,
        )

    def parse_setlike_declaration(self, readonly: bool = False) -> SetlikeDeclaration:
        self.consume_keyword("setlike")
        parameters = self.parse_type_parameters()
        if len(parameters) != 1:
            self.raise_parse_error("expected one setlike type parameter")
        self.consume_member_semicolon()

        return SetlikeDeclaration(
            value_type=parameters[0],
            readonly=readonly,
        )

    def parse_special_operation(
        self,
        keyword: str,
        extended_attributes: Dict[str, str],
    ) -> SpecialOperation:
        self.consume_keyword(keyword)
        self.consume_whitespace()

        return_type = self.parse_type()
        self.consume_whitespace()

        name = ""
        if not self.lexer.next_is("("):
            name = self.parse_identifier()
            self.consume_whitespace()

        parameters = self.parse_parenthesized_parameters()
        self.consume_member_semicolon()

        if not parameters:
            self.raise_parse_error("special operation must have an identifier parameter")

        return SpecialOperation(
            identifier_type=parameters[0].type,
            return_type=return_type,
            name=name,
            parameters=parameters,
            extended_attributes=extended_attributes,
        )

    def parse_parenthesized_parameters(self) -> List[OperationParameter]:
        self.assert_specific("(")
        self.consume_whitespace()

        parameters: List[OperationParameter] = []
        if not self.lexer.next_is(")"):
            parameters = self.parse_operation_parameters()
            self.consume_whitespace()

        self.assert_specific(")")
        self.consume_whitespace()
        return parameters

    def parse_operation_parameters(self) -> List[OperationParameter]:
        parameters: List[OperationParameter] = []

        self.consume_whitespace()
        while not self.lexer.next_is(")"):
            extended_attributes = self.parse_leading_extended_attributes()

            optional = self.consume_optional_keyword("optional")

            extended_attributes.update(self.parse_leading_extended_attributes())

            parameter_type = self.parse_type()
            self.consume_whitespace()

            variadic = self.lexer.consume_specific("...")
            self.consume_whitespace()

            parameter_name = self.parse_identifier()

            self.consume_whitespace()
            default_value: Optional[str] = None
            if self.lexer.consume_specific("="):
                default_value = self.consume_until_top_level(",", ")").strip()
                self.consume_whitespace()
            parameters.append(
                OperationParameter(
                    name=parameter_name,
                    type=parameter_type,
                    optional=optional,
                    default_value=default_value,
                    variadic=variadic,
                    extended_attributes=extended_attributes,
                )
            )
            if not self.lexer.consume_specific(","):
                break
            self.consume_whitespace()

        return parameters

    def parse_operation(self, extended_attributes: Dict[str, str]) -> Operation:
        return_type = self.parse_type()
        self.consume_whitespace()
        name = self.parse_identifier()
        self.consume_whitespace()
        parameters = self.parse_parenthesized_parameters()
        self.consume_member_semicolon()

        return Operation(
            name=name,
            return_type=return_type,
            parameters=parameters,
            extended_attributes=extended_attributes,
        )

    def parse_constructor(self, extended_attributes: Dict[str, str]) -> Constructor:
        self.consume_keyword("constructor")
        self.consume_whitespace()
        parameters = self.parse_parenthesized_parameters()
        self.consume_member_semicolon()

        return Constructor(
            parameters=parameters,
            extended_attributes=extended_attributes,
        )

    def parse_constant(self) -> Constant:
        self.consume_keyword("const")
        self.consume_whitespace()

        constant_type = self.parse_type()
        self.consume_whitespace()

        name = self.parse_identifier()
        self.consume_whitespace()
        self.assert_specific("=")
        self.consume_whitespace()

        value = self.consume_until_top_level(";").strip()
        self.consume_member_semicolon()

        return Constant(
            type=constant_type,
            name=name,
            value=value,
        )

    def parse_attribute(
        self,
        extended_attributes: Dict[str, str],
        readonly: bool = False,
        inherit: bool = False,
    ) -> Attribute:
        self.consume_keyword("attribute")
        self.consume_whitespace()

        attribute_type = self.parse_type()
        self.consume_whitespace()

        name = self.parse_identifier()
        self.consume_member_semicolon()

        return Attribute(
            name=name,
            type=attribute_type,
            readonly=readonly,
            inherit=inherit,
            extended_attributes=extended_attributes,
        )

    def parse_extended_attributes(self) -> Dict[str, str]:
        extended_attributes: Dict[str, str] = {}

        while True:
            self.consume_whitespace()
            if self.lexer.consume_specific("]"):
                break

            name = self.parse_identifier()
            value = ""
            if self.lexer.consume_specific("="):
                value = self.consume_until_top_level(",", "]").strip()

            extended_attributes[name] = value
            self.lexer.consume_specific(",")

        self.consume_whitespace()
        return extended_attributes

    def parse_leading_extended_attributes(self) -> Dict[str, str]:
        self.consume_whitespace()
        if self.lexer.consume_specific("["):
            return self.parse_extended_attributes()
        return {}

    def consume_member_semicolon(self) -> None:
        self.consume_whitespace()
        self.assert_specific(";")

    def consume_until_top_level(self, *terminators: str) -> str:
        start = self.lexer.tell()
        delimiter_stack: List[str] = []
        active_quote = ""
        matching_delimiter = {
            "(": ")",
            "[": "]",
            "{": "}",
            "<": ">",
        }

        while not self.lexer.is_eof():
            character = self.lexer.peek()

            if active_quote:
                self.lexer.consume()
                if character == active_quote:
                    active_quote = ""
                continue

            if character in ('"', "'"):
                active_quote = character
                self.lexer.consume()
                continue

            if character in terminators and not delimiter_stack:
                return self.contents[start : self.lexer.tell()]

            character = self.lexer.consume()
            if character in matching_delimiter:
                delimiter_stack.append(matching_delimiter[character])
            elif delimiter_stack and character == delimiter_stack[-1]:
                delimiter_stack.pop()

        return self.contents[start : self.lexer.tell()]

    def parse_identifier(self) -> str:
        identifier = self.lexer.consume_while(lambda character: character.isalnum() or character in "_-")
        if not identifier:
            self.raise_parse_error("expected identifier")
        return identifier.lstrip("_")

    def consume_whitespace(self) -> None:
        consumed = True
        while consumed:
            consumed = False

            whitespace = self.lexer.consume_while(str.isspace)
            if whitespace:
                consumed = True

            if self.lexer.consume_specific("//"):
                self.lexer.ignore_until("\n")
                if self.lexer.peek() == "\n":
                    self.lexer.ignore()
                consumed = True

    def next_is_keyword(self, keyword: str) -> bool:
        if not self.lexer.next_is(keyword):
            return False

        end = self.lexer.tell() + len(keyword)
        if end >= len(self.contents):
            return True

        trailing_character = self.contents[end]
        return trailing_character.isspace() or trailing_character in "{([;:<"

    def consume_optional_keyword(self, keyword: str) -> bool:
        if not self.next_is_keyword(keyword):
            return False

        self.consume_keyword(keyword)
        self.consume_whitespace()
        return True

    def consume_keyword(self, keyword: str) -> None:
        if not self.next_is_keyword(keyword):
            self.raise_parse_error(f"expected '{keyword}'")
        self.lexer.ignore(len(keyword))

    def assert_specific(self, expected_character: str) -> None:
        if not self.lexer.consume_specific(expected_character):
            self.raise_parse_error(f"expected '{expected_character}'")

    def raise_parse_error(self, message: str) -> NoReturn:
        line_number = 1
        column_number = 1

        for index, character in enumerate(self.contents):
            if index == self.lexer.tell():
                break
            if character == "\n":
                line_number += 1
                column_number = 1
            else:
                column_number += 1

        raise ParseError(f"{self.path}:{line_number}:{column_number}: error: {message}")
