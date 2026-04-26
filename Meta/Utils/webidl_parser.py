# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from dataclasses import dataclass
from dataclasses import field
from pathlib import Path
from typing import Dict
from typing import List
from typing import Optional

from Utils.lexer import Lexer


@dataclass
class Constant:
    declaration: str


@dataclass
class SpecialOperation:
    identifier_type: str
    declaration: str


@dataclass
class Interface:
    name: str
    path: Path
    extended_attributes: Dict[str, str] = field(default_factory=dict)
    is_namespace: bool = False
    is_callback_interface: bool = False
    parent_name: str = ""
    constants: List[Constant] = field(default_factory=list)
    named_property_getter: Optional[SpecialOperation] = None
    indexed_property_getter: Optional[SpecialOperation] = None
    implemented_name: str = ""
    namespaced_name: str = ""
    constructor_class: str = ""
    prototype_class: str = ""
    namespace_class: str = ""

    def supports_named_properties(self) -> bool:
        return self.named_property_getter is not None

    def finalize(self) -> None:
        legacy_namespace = self.extended_attributes.get("LegacyNamespace")
        if legacy_namespace:
            self.namespaced_name = f"{legacy_namespace}.{self.name}"
        else:
            self.namespaced_name = self.name

        self.implemented_name = self.extended_attributes.get("ImplementedAs", self.name)
        self.constructor_class = f"{self.implemented_name}Constructor"
        self.prototype_class = f"{self.implemented_name}Prototype"
        self.namespace_class = f"{self.name}Namespace"


@dataclass
class Module:
    path: Path
    interface: Optional[Interface] = None


@dataclass
class NestingState:
    parentheses_depth: int = 0
    bracket_depth: int = 0
    brace_depth: int = 0
    angle_depth: int = 0

    def is_at_top_level(self) -> bool:
        return not any((self.parentheses_depth, self.bracket_depth, self.brace_depth, self.angle_depth))

    def update_for_character(self, character: str) -> None:
        if character == "(":
            self.parentheses_depth += 1
        elif character == ")":
            self.parentheses_depth = max(self.parentheses_depth - 1, 0)
        elif character == "[":
            self.bracket_depth += 1
        elif character == "]":
            self.bracket_depth = max(self.bracket_depth - 1, 0)
        elif character == "{":
            self.brace_depth += 1
        elif character == "}":
            self.brace_depth = max(self.brace_depth - 1, 0)
        elif character == "<":
            self.angle_depth += 1
        elif character == ">":
            self.angle_depth = max(self.angle_depth - 1, 0)


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
            extended_attributes: Dict[str, str] = {}
            if self.lexer.consume_specific("["):
                extended_attributes = self.parse_extended_attributes()

            if self.next_is_keyword("dictionary") or self.next_is_keyword("partial dictionary"):
                self.skip_braced_declaration()
            elif self.next_is_keyword("enum"):
                self.skip_braced_declaration()
            elif self.next_is_keyword("typedef"):
                self.consume_keyword("typedef")
                self.consume_statement_text()
            elif self.next_is_keyword("partial interface mixin"):
                self.skip_braced_declaration()
            elif self.next_is_keyword("partial interface"):
                self.skip_braced_declaration()
            elif self.next_is_keyword("interface mixin"):
                self.skip_braced_declaration()
            elif self.next_is_keyword("partial namespace"):
                self.skip_braced_declaration()
            elif self.next_is_keyword("callback interface"):
                module.interface = self.set_or_check_module_interface(
                    module.interface,
                    self.parse_interface(extended_attributes, is_callback_interface=True),
                )
            elif self.next_is_keyword("callback"):
                self.consume_keyword("callback")
                self.consume_statement_text()
            elif self.next_is_keyword("namespace"):
                module.interface = self.set_or_check_module_interface(
                    module.interface,
                    self.parse_interface(extended_attributes, is_namespace=True),
                )
            elif self.next_is_keyword("interface"):
                module.interface = self.set_or_check_module_interface(
                    module.interface,
                    self.parse_interface(extended_attributes),
                )
            else:
                self.parse_includes_statement()

            self.consume_whitespace()

        if module.interface is None:
            self.raise_parse_error("did not find an interface, callback interface, or namespace")

        return module

    def set_or_check_module_interface(
        self, existing_interface: Optional[Interface], candidate_interface: Interface
    ) -> Interface:
        if existing_interface is None:
            return candidate_interface

        if not existing_interface.is_namespace and existing_interface.name == candidate_interface.name:
            return existing_interface

        self.raise_parse_error(
            "encountered multiple interface, callback interface, or namespace declarations in one file"
        )

    def parse_includes_statement(self) -> None:
        self.parse_identifier_ending_with_space()
        self.consume_whitespace()

        if not self.next_is_keyword("includes"):
            self.raise_parse_error("expected a declaration or an includes statement")

        self.consume_keyword("includes")
        self.consume_whitespace()
        self.parse_identifier_ending_with_space_or(";")
        self.consume_whitespace()
        self.assert_specific(";")

    def parse_interface(
        self,
        extended_attributes: Dict[str, str],
        is_namespace: bool = False,
        is_callback_interface: bool = False,
    ) -> Interface:
        if is_callback_interface:
            self.consume_keyword("callback")
            self.consume_whitespace()
            self.consume_keyword("interface")
        elif is_namespace:
            self.consume_keyword("namespace")
        else:
            self.consume_keyword("interface")

        self.consume_whitespace()

        interface = Interface(
            name=self.parse_identifier_ending_with_space_or(":", "{"),
            path=self.path,
            extended_attributes=extended_attributes,
            is_namespace=is_namespace,
            is_callback_interface=is_callback_interface,
        )

        self.consume_whitespace()
        if not is_namespace and self.lexer.consume_specific(":"):
            self.consume_whitespace()
            interface.parent_name = self.parse_identifier_ending_with_space_or("{")
            self.consume_whitespace()

        body_text = self.consume_braced_block()
        self.consume_whitespace()
        self.assert_specific(";")

        if not is_namespace:
            self.parse_interface_body(interface, body_text)

        interface.finalize()
        return interface

    def parse_interface_body(self, interface: Interface, body_text: str) -> None:
        for statement in split_top_level_statements(remove_line_comments(body_text)):
            if not statement:
                continue

            stripped_statement = strip_leading_extended_attributes(statement).strip()
            if not stripped_statement:
                continue

            if stripped_statement.startswith("const "):
                interface.constants.append(Constant(stripped_statement))
                continue

            if stripped_statement.startswith("getter "):
                identifier_type = parse_special_operation_identifier_type(stripped_statement)
                special_operation = SpecialOperation(identifier_type=identifier_type, declaration=stripped_statement)

                if identifier_type == "DOMString":
                    interface.named_property_getter = special_operation
                elif identifier_type == "unsigned long":
                    interface.indexed_property_getter = special_operation
                else:
                    self.raise_parse_error(
                        f"named/indexed property getter must use DOMString or unsigned long, got '{identifier_type}'"
                    )

    def parse_extended_attributes(self) -> Dict[str, str]:
        extended_attributes: Dict[str, str] = {}

        while True:
            self.consume_whitespace()
            if self.lexer.consume_specific("]"):
                break

            name = self.parse_identifier_ending_with_space_or("]", "=", ",")
            value = ""
            if self.lexer.consume_specific("="):
                value = self.consume_extended_attribute_value().strip()

            extended_attributes[name] = value
            self.lexer.consume_specific(",")

        self.consume_whitespace()
        return extended_attributes

    def consume_extended_attribute_value(self) -> str:
        start = self.lexer.tell()
        parentheses_depth = 0

        while not self.lexer.is_eof():
            character = self.lexer.peek()
            if character == "(":
                parentheses_depth += 1
            elif character == ")":
                if parentheses_depth > 0:
                    parentheses_depth -= 1
            elif parentheses_depth == 0 and character in (",", "]"):
                break

            self.lexer.consume()

        return self.contents[start : self.lexer.tell()]

    def skip_braced_declaration(self) -> None:
        while not self.lexer.is_eof() and self.lexer.peek() != "{":
            self.lexer.consume()

        if self.lexer.is_eof():
            self.raise_parse_error("expected '{' while skipping declaration")

        self.consume_braced_block()
        self.consume_whitespace()
        self.assert_specific(";")

    def consume_braced_block(self) -> str:
        self.assert_specific("{")

        start = self.lexer.tell()
        brace_depth = 1
        active_quote = ""

        while not self.lexer.is_eof():
            character = self.lexer.consume()

            if active_quote:
                if character == active_quote:
                    active_quote = ""
                continue

            if character == "/" and self.lexer.peek() == "/":
                self.lexer.ignore_until("\n")
                if self.lexer.peek() == "\n":
                    self.lexer.ignore()
                continue

            if character in ('"', "'"):
                active_quote = character
                continue

            if character == "{":
                brace_depth += 1
            elif character == "}":
                brace_depth -= 1
                if brace_depth == 0:
                    end = self.lexer.tell() - 1
                    return self.contents[start:end]

        self.raise_parse_error("unterminated declaration body")

    def consume_statement_text(self) -> str:
        start = self.lexer.tell()
        nesting_state = NestingState()
        active_quote = ""

        while not self.lexer.is_eof():
            character = self.lexer.consume()

            if active_quote:
                if character == active_quote:
                    active_quote = ""
                continue

            if character == "/" and self.lexer.peek() == "/":
                self.lexer.ignore_until("\n")
                if self.lexer.peek() == "\n":
                    self.lexer.ignore()
                continue

            if character in ('"', "'"):
                active_quote = character
                continue

            if character == ";" and nesting_state.is_at_top_level():
                end = self.lexer.tell() - 1
                return self.contents[start:end]
            nesting_state.update_for_character(character)

        self.raise_parse_error("unterminated statement")

    def parse_identifier_ending_with_space(self) -> str:
        return self.parse_identifier_ending_with_space_or()

    def parse_identifier_ending_with_space_or(self, *terminators: str) -> str:
        identifier = self.lexer.consume_until(lambda character: character.isspace() or character in terminators)
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
        return trailing_character.isspace() or trailing_character in "{([;:"

    def consume_keyword(self, keyword: str) -> None:
        if not self.next_is_keyword(keyword):
            self.raise_parse_error(f"expected '{keyword}'")
        self.lexer.ignore(len(keyword))

    def assert_specific(self, expected_character: str) -> None:
        if not self.lexer.consume_specific(expected_character):
            self.raise_parse_error(f"expected '{expected_character}'")

    def raise_parse_error(self, message: str) -> None:
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


def remove_line_comments(text: str) -> str:
    lines = []
    for line in text.splitlines():
        comment_index = line.find("//")
        if comment_index != -1:
            line = line[:comment_index]
        lines.append(line)
    return "\n".join(lines)


def split_top_level_statements(text: str) -> List[str]:
    statements: List[str] = []
    start = 0
    nesting_state = NestingState()
    active_quote = ""

    for index, character in enumerate(text):
        if active_quote:
            if character == active_quote:
                active_quote = ""
            continue

        if character in ('"', "'"):
            active_quote = character
            continue

        if character == ";" and nesting_state.is_at_top_level():
            statements.append(text[start:index].strip())
            start = index + 1
            continue

        nesting_state.update_for_character(character)

    trailing_text = text[start:].strip()
    if trailing_text:
        statements.append(trailing_text)

    return statements


def strip_leading_extended_attributes(text: str) -> str:
    remaining_text = text.lstrip()
    while remaining_text.startswith("["):
        closing_bracket = find_matching_closer(remaining_text, 0, "[", "]")
        remaining_text = remaining_text[closing_bracket + 1 :].lstrip()
    return remaining_text


def parse_special_operation_identifier_type(statement: str) -> str:
    parenthesized_text = extract_final_parenthesized_group(statement)
    parameter_text = split_top_level_items(parenthesized_text, ",")[0].strip()
    parameter_text = strip_leading_extended_attributes(parameter_text)

    last_space = find_last_top_level_whitespace(parameter_text)
    if last_space == -1:
        raise ParseError(f"could not determine getter identifier type from '{statement}'")

    return normalize_whitespace(parameter_text[:last_space])


def extract_final_parenthesized_group(text: str) -> str:
    stripped_text = text.rstrip()
    if not stripped_text.endswith(")"):
        raise ParseError(f"expected a trailing parameter list in '{text}'")

    closing_index = len(stripped_text) - 1
    opening_index = find_matching_opener(stripped_text, closing_index, "(", ")")
    return stripped_text[opening_index + 1 : closing_index]


def split_top_level_items(text: str, delimiter: str) -> List[str]:
    items: List[str] = []
    start = 0
    nesting_state = NestingState()
    active_quote = ""

    for index, character in enumerate(text):
        if active_quote:
            if character == active_quote:
                active_quote = ""
            continue

        if character in ('"', "'"):
            active_quote = character
            continue

        if character == delimiter and nesting_state.is_at_top_level():
            items.append(text[start:index])
            start = index + 1
            continue

        nesting_state.update_for_character(character)

    items.append(text[start:])
    return items


def find_last_top_level_whitespace(text: str) -> int:
    last_space = -1
    nesting_state = NestingState()

    for index, character in enumerate(text):
        if character.isspace() and nesting_state.is_at_top_level():
            last_space = index
        nesting_state.update_for_character(character)

    return last_space


def find_matching_closer(text: str, start_index: int, opener: str, closer: str) -> int:
    depth = 0
    for index in range(start_index, len(text)):
        character = text[index]
        if character == opener:
            depth += 1
        elif character == closer:
            depth -= 1
            if depth == 0:
                return index

    raise ParseError(f"unterminated '{opener}' in '{text}'")


def find_matching_opener(text: str, start_index: int, opener: str, closer: str) -> int:
    depth = 0
    for index in range(start_index, -1, -1):
        character = text[index]
        if character == closer:
            depth += 1
        elif character == opener:
            depth -= 1
            if depth == 0:
                return index

    raise ParseError(f"unterminated '{opener}' in '{text}'")


def normalize_whitespace(text: str) -> str:
    return " ".join(text.split())
