#!/usr/bin/env python3

# Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause


import argparse
import io
import re
import sys
import textwrap

from dataclasses import dataclass
from dataclasses import field
from pathlib import Path
from typing import List
from typing import Optional
from typing import TextIO
from typing import Tuple

sys.path.append(str(Path(__file__).resolve().parent.parent))

from Utils.lexer import Lexer
from Utils.utils import string_hash

AK_DIRECTORY = Path(__file__).resolve().parent.parent.parent / "AK"

PRIMITIVE_TYPES = {
    "i8",
    "u8",
    "i16",
    "u16",
    "i32",
    "u32",
    "i64",
    "u64",
    "int",
    "unsigned",
    "unsigned int",
    "size_t",
    "bool",
    "double",
    "float",
}

SIMPLE_TYPES = {
    "ReadonlyBytes",
    "StringView",
    "AK::CaseSensitivity",
    "AK::Duration",
    "Audio::Audibility",
    "Core::File::OpenMode",
    "Gfx::Color",
    "Gfx::FloatPoint",
    "Gfx::FloatSize",
    "Gfx::IntPoint",
    "Gfx::IntSize",
    "HTTP::Cookie::Source",
    "Web::DevicePixelPoint",
    "Web::DevicePixelRect",
    "Web::DevicePixels",
    "Web::DevicePixelSize",
    "Web::EventResult",
    "Web::HTML::AllowMultipleFiles",
    "Web::HTML::HistoryHandlingBehavior",
    "Web::HTML::VisibilityState",
    "WebView::PageInfoType",
}


@dataclass
class Parameter:
    attributes: List[str] = field(default_factory=list)
    type: str = ""
    type_for_encoding: str = ""
    name: str = ""


@dataclass
class Message:
    attributes: List[str] = field(default_factory=list)
    name: str = ""
    is_synchronous: bool = False
    inputs: List[Parameter] = field(default_factory=list)
    outputs: List[Parameter] = field(default_factory=list)

    def response_name(self) -> str:
        return f"{pascal_case(self.name)}Response"


@dataclass
class Endpoint:
    name: str = ""
    magic: int = 0
    messages: List[Message] = field(default_factory=list)
    # The argument a message can be routed by: messages whose first parameter is this one are about the
    # object it names, so a receiver can hand them to that object and a sender can bind a proxy to it.
    route: Optional[Parameter] = None
    export_macro: str = ""

    def binds(self, message: Message) -> bool:
        if self.route is None or not message.inputs:
            return False
        return message.inputs[0].type == self.route.type and message.inputs[0].name == self.route.name

    def routes(self, message: Message) -> bool:
        # A message the connection answers itself stays on the connection, whatever it starts with. It is still
        # bound: what the receiver does with a message says nothing about how a sender names its object.
        if "NotRouted" in message.attributes:
            return False
        return self.binds(message)

    def validate(self) -> None:
        if self.route is None:
            return
        for message in self.messages:
            if not self.routes(message):
                continue
            # A routed message whose object is gone is dropped, and a synchronous sender waits for an answer. An
            # empty one acknowledges; one carrying values would be invented, and the zero of a type is an answer.
            if message.is_synchronous and message.outputs:
                raise RuntimeError(
                    f"{self.name}::{message.name} is synchronous and returns values, so it cannot be routed: "
                    f"mark it [NotRouted] and answer it on the connection"
                )

    def route_scope(self) -> str:
        assert self.route is not None
        name = self.route.name
        if name.endswith("_id"):
            name = name[: -len("_id")]
        return name

    def route_scope_pascal(self) -> str:
        return pascal_case(self.route_scope())


INTEGER_TYPES = {"u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64"}

# The attributes a struct field may have, and the types each one applies to.
FIELD_ATTRIBUTE_TYPES = {
    "MaxBytes": {"String", "ByteString", "ByteBuffer"},
    "MaxCount": {"Vector"},
    "NonZero": INTEGER_TYPES,
    "Finite": {"float", "double"},
}

IDENTIFIER = r"[A-Za-z_][A-Za-z_0-9]*"
QUALIFIED_NAME = re.compile(rf"{IDENTIFIER}(::{IDENTIFIER})*")
FIELD_TYPE = re.compile(rf"(::)?{IDENTIFIER}(::{IDENTIFIER})*(<.+>)?")

# Spellings of the same C++ type, which must not be told apart.
TYPE_ALIASES = {"int": "i32", "unsigned": "u32", "size_t": "u64"}


def canonical_type(type: str) -> str:
    """The type with one spelling for each C++ type: without leading "::", and with sized integer names."""
    type = re.sub(r"(?<![\w:])::", "", type)
    type = re.sub(r"\bunsigned int\b", "u32", type)
    return re.sub(r"(?<![\w:])\w+(?![\w:])", lambda name: TYPE_ALIASES.get(name[0], name[0]), type)


def field_attributes(member: Parameter) -> List[Tuple[str, str]]:
    """The attributes of a struct field as (name, argument) pairs, e.g. ("MaxBytes", "64")."""
    attributes: List[Tuple[str, str]] = []
    for attribute in member.attributes:
        name, _, argument = attribute.partition("=")
        attributes.append((name.strip(), argument.strip()))
    return attributes


@dataclass
class FieldRefusal:
    """A value of a field that its attributes refuse: how C++ and Rust find it, and what is wrong with it."""

    attribute: str
    cpp_condition: str
    rust_condition: str
    meaning: str

    def is_checked_after_decoding(self) -> bool:
        # NB: Decoders check sizes before they decode the contents.
        return self.attribute not in ("MaxBytes", "MaxCount")


def field_refusals(member: Parameter, cpp_value: str, rust_value: str) -> List[FieldRefusal]:
    refusals: List[FieldRefusal] = []
    for name, argument in field_attributes(member):
        if name == "NonZero":
            refusals.append(FieldRefusal(name, f"{cpp_value} == 0", f"{rust_value} == 0", "is zero"))
        elif name == "Finite":
            refusals.append(
                FieldRefusal(name, f"!isfinite({cpp_value})", f"!{rust_value}.is_finite()", "is not finite")
            )
        elif name in ("MaxBytes", "MaxCount"):
            cpp_size = {"String": "bytes().size()", "ByteString": "length()"}.get(member.type, "size()")
            rust_size = {"String": "as_bytes().len()", "ByteString": "0.len()"}.get(member.type, "len()")
            refusals.append(
                FieldRefusal(
                    name,
                    f"{cpp_value}.{cpp_size} > {argument}",
                    f"{rust_value}.{rust_size} > {argument}",
                    "is too large",
                )
            )
    return refusals


@dataclass
class Declaration:
    """A type declared in an IPC file: a struct, an enum or a variant."""

    kind: str = ""
    # The C++ name, including its namespace, e.g. "WebView::DictionaryLookup".
    name: str = ""
    attributes: List[str] = field(default_factory=list)
    # The integer type an enum goes as.
    underlying_type: str = ""
    # A struct's fields, an enum's values (with names only), or a variant's alternatives (with a name for Rust).
    members: List[Parameter] = field(default_factory=list)

    def is_existing_cpp(self) -> bool:
        # The C++ type is defined elsewhere, so only its codec is generated.
        # NB: Nothing can check that the declaration of an existing struct lists every field of its C++ type, so a
        #     field it leaves out is not sent, and the decoder leaves it to its default. The declaration must list
        #     them all, in their C++ order.
        return "ExistingCpp" in self.attributes

    def rust_name(self) -> str:
        return self.name.replace("::", "_")

    def has_accessors(self) -> bool:
        # The existing C++ type is read through accessor functions and built with a constructor taking every field.
        return "Accessors" in self.attributes

    def validate(self) -> None:
        if not re.fullmatch(rf"{IDENTIFIER}(::{IDENTIFIER})+", self.name):
            raise RuntimeError(f"Expected a type name with a namespace, got '{self.name}'")
        for attribute in self.attributes:
            if attribute not in ("ExistingCpp", "Accessors"):
                raise RuntimeError(f"Unknown attribute '{attribute}' on {self.name}")
        if self.has_accessors() and (self.kind != "struct" or not self.is_existing_cpp()):
            raise RuntimeError(f"Only an existing C++ struct can have accessors: {self.name}")

        names = [member.name for member in self.members]
        for name in names:
            if not re.fullmatch(IDENTIFIER, name):
                raise RuntimeError(f"Invalid member name '{name}' in {self.name}")
        if self.kind != "enum":
            for member in self.members:
                if not FIELD_TYPE.fullmatch(member.type):
                    raise RuntimeError(f"Invalid type '{member.type}' of {self.name}::{member.name}")
        if len(set(names)) != len(names):
            raise RuntimeError(f"Duplicate member name in {self.name}")
        if self.kind != "struct" and not names:
            raise RuntimeError(f"Empty {self.kind}: {self.name}")

        if self.kind == "enum":
            if self.underlying_type not in INTEGER_TYPES:
                raise RuntimeError(f"Enum {self.name} needs a sized integer type, got '{self.underlying_type}'")
            value_bits = int(self.underlying_type[1:]) - (1 if self.underlying_type.startswith("i") else 0)
            if len(names) > 2**value_bits:
                raise RuntimeError(f"Enum {self.name} has more values than {self.underlying_type} holds")
        elif self.kind == "variant":
            # NB: An AK::Variant of fewer than 255 types goes with a u8 index.
            if len(names) >= 255:
                raise RuntimeError(f"Variant {self.name} has too many alternatives")
            if len({canonical_type(member.type) for member in self.members}) != len(self.members):
                raise RuntimeError(f"Variant {self.name} needs a distinct type for each alternative")
        else:
            for member in self.members:
                for name, argument in field_attributes(member):
                    if name not in FIELD_ATTRIBUTE_TYPES:
                        raise RuntimeError(f"Unknown attribute '{name}' on {self.name}::{member.name}")
                    if member.type.partition("<")[0] not in FIELD_ATTRIBUTE_TYPES[name]:
                        raise RuntimeError(f"{name} cannot apply to {self.name}::{member.name} of type {member.type}")
                    if name in ("MaxBytes", "MaxCount"):
                        if not argument.isdecimal() or not 0 < int(argument) <= 0xFFFFFFFF:
                            raise RuntimeError(f"{name} on {self.name}::{member.name} needs a size, got '{argument}'")
                    elif argument:
                        raise RuntimeError(f"{name} on {self.name}::{member.name} takes no argument")


@dataclass
class Document:
    includes: List[str] = field(default_factory=list)
    declarations: List[Declaration] = field(default_factory=list)
    endpoints: List[Endpoint] = field(default_factory=list)


def is_primitive_type(type: str) -> bool:
    return type in PRIMITIVE_TYPES


def is_simple_type(type: str) -> bool:
    if type.startswith("ReadonlySpan<") and type.endswith(">"):
        return True
    return type in SIMPLE_TYPES


def is_primitive_or_simple_type(type: str) -> bool:
    return is_primitive_type(type) or is_simple_type(type)


def pascal_case(identifier: str) -> str:
    words = identifier.split("_")
    return "".join(word[0].title() + word[1:] for word in words)


def message_name_qualified(endpoint: str, message: str, is_response: bool) -> str:
    message = pascal_case(message)
    response = "Response" if is_response else ""

    return f"Messages::{endpoint}::{message}{response}"


def make_argument_type(type: str) -> str:
    if is_primitive_or_simple_type(type):
        return type
    return f"{type} const&"


def parse(contents: str) -> Document:
    lexer = Lexer(contents)
    document = Document()
    endpoints = document.endpoints

    def assert_specific(ch: str) -> None:
        if not lexer.consume_specific(ch):
            raise RuntimeError(f"Expected '{ch}' at position {lexer.position}, got '{lexer.peek()}'")

    def consume_whitespace() -> None:
        lexer.ignore_while(lambda c: c.isspace())
        while lexer.next_is("//"):
            lexer.ignore_until("\n")
            lexer.ignore_while(lambda c: c.isspace())

    def parse_parameter_type(delimiters: str = "") -> str:
        parameter_type = lexer.consume_until(lambda c: c == "<" or c.isspace() or c in delimiters)

        if lexer.peek() == "<":
            lexer.consume()

            parts: List[str] = [parameter_type, "<"]
            nesting = 1

            while nesting > 0:
                inner = lexer.consume_until(lambda c: c in ("<", ">"))
                if lexer.is_eof():
                    raise RuntimeError("Unexpected EOF when parsing parameter type")

                parts.append(inner)

                if lexer.peek() == "<":
                    nesting += 1
                elif lexer.peek() == ">":
                    nesting -= 1

                parts.append(lexer.consume())

            parameter_type = "".join(parts)

        return parameter_type

    def parse_attributes(storage: List[str]) -> None:
        if not lexer.consume_specific("["):
            return

        while True:
            if lexer.is_eof():
                raise RuntimeError("Unexpected EOF in attributes")

            if lexer.consume_specific("]"):
                consume_whitespace()
                break

            if lexer.consume_specific(","):
                consume_whitespace()

            storage.append(lexer.consume_until(lambda c: c in ("]", ",")).strip())
            consume_whitespace()

    def parse_parameter(storage: List[Parameter], message_name: str) -> None:
        if lexer.is_eof():
            raise RuntimeError("EOF when parsing parameter")

        parameter = Parameter()
        parameter_index = len(storage) + 1

        parse_attributes(parameter.attributes)

        parameter.type = parse_parameter_type()
        if parameter.type.endswith(",") or parameter.type.endswith(")"):
            raise RuntimeError(f"Parameter {parameter_index} of method: {message_name} must be named")

        if parameter.type.startswith("Vector<") and parameter.type.endswith(">"):
            parameter.type_for_encoding = "ReadonlySpan" + parameter.type[len("Vector") :]
        elif parameter.type in ("String", "ByteString"):
            parameter.type_for_encoding = "StringView"
        elif parameter.type == "Utf16String":
            parameter.type_for_encoding = "Utf16View"
        elif parameter.type == "ByteBuffer":
            parameter.type_for_encoding = "ReadonlyBytes"
        else:
            parameter.type_for_encoding = parameter.type

        if lexer.is_eof():
            raise RuntimeError("Unexpected EOF when parsing parameter")

        consume_whitespace()
        parameter.name = lexer.consume_until(lambda c: c.isspace() or c == "," or c == ")")
        consume_whitespace()

        storage.append(parameter)

    def parse_parameters(storage: List[Parameter], message_name: str) -> None:
        while True:
            consume_whitespace()
            if lexer.peek() == ")":
                break

            parse_parameter(storage, message_name)
            consume_whitespace()

            if lexer.consume_specific(","):
                continue
            if lexer.peek() == ")":
                break

    def parse_message() -> None:
        message = Message()

        consume_whitespace()
        parse_attributes(message.attributes)
        message.name = lexer.consume_until(lambda c: c.isspace() or c == "(")

        consume_whitespace()
        assert_specific("(")
        parse_parameters(message.inputs, message.name)
        assert_specific(")")
        consume_whitespace()

        assert_specific("=")
        type = lexer.consume()

        if type == ">":
            message.is_synchronous = True
        elif type == "|":
            message.is_synchronous = False
        else:
            raise RuntimeError(f"Unexpected message type: '={type}'")

        consume_whitespace()

        if message.is_synchronous:
            assert_specific("(")
            parse_parameters(message.outputs, message.name)
            assert_specific(")")

        consume_whitespace()
        endpoints[-1].messages.append(message)

    def parse_messages() -> None:
        while True:
            consume_whitespace()
            if lexer.peek() == "}":
                break
            parse_message()
            consume_whitespace()

    def parse_include() -> None:
        consume_whitespace()
        include = lexer.consume_while(lambda c: c != "\n")
        consume_whitespace()

        document.includes.append(include)

    def parse_includes() -> None:
        while True:
            consume_whitespace()
            if lexer.peek() != "#":
                break
            parse_include()
            consume_whitespace()

    def parse_endpoint() -> None:
        endpoints.append(Endpoint())

        consume_whitespace()
        endpoints[-1].name = lexer.consume_while(lambda c: not c.isspace())
        endpoints[-1].magic = string_hash(endpoints[-1].name)

        consume_whitespace()
        if lexer.consume_specific("routed"):
            consume_whitespace()
            if not lexer.consume_specific("by"):
                raise RuntimeError(f"Expected 'by' after 'routed' at position {lexer.position}")
            consume_whitespace()
            route = Parameter()
            route.type = parse_parameter_type()
            route.type_for_encoding = route.type
            consume_whitespace()
            route.name = lexer.consume_until(lambda c: c.isspace() or c == "{")
            if not route.name:
                raise RuntimeError(f"Expected the name of the routing argument at position {lexer.position}")
            endpoints[-1].route = route
            consume_whitespace()
        assert_specific("{")
        parse_messages()
        assert_specific("}")
        consume_whitespace()

    def parse_declaration(kind: str, attributes: List[str]) -> None:
        declaration = Declaration(kind=kind, attributes=attributes)
        consume_whitespace()
        name = QUALIFIED_NAME.match(lexer.text, lexer.position)
        declaration.name = name[0] if name else ""
        lexer.ignore(len(declaration.name))
        consume_whitespace()
        if kind == "enum":
            assert_specific(":")
            consume_whitespace()
            declaration.underlying_type = lexer.consume_until(lambda c: c.isspace() or c == "{")
            consume_whitespace()
        assert_specific("{")

        while True:
            consume_whitespace()
            if lexer.consume_specific("}"):
                break
            if lexer.is_eof():
                raise RuntimeError(f"Unexpected EOF in {kind} {declaration.name}")

            # Struct fields are declared as in C++, "Type name;". Enum values are listed by name and variant
            # alternatives as "Name Type", separated by commas.
            member = Parameter()
            if kind == "struct":
                parse_attributes(member.attributes)
                member.type = parse_parameter_type()
                consume_whitespace()
                member.name = lexer.consume_until(lambda c: c.isspace() or c == ";")
                consume_whitespace()
                assert_specific(";")
            else:
                member.name = lexer.consume_until(lambda c: c.isspace() or c in ",}")
                consume_whitespace()
                if kind == "variant":
                    member.type = parse_parameter_type(",}")
                    consume_whitespace()
                if not lexer.consume_specific(",") and lexer.peek() != "}":
                    raise RuntimeError(f"Expected ',' or '}}' at position {lexer.position}, got '{lexer.peek()}'")
            declaration.members.append(member)

        declaration.validate()
        for existing in document.declarations:
            if existing.name == declaration.name:
                raise RuntimeError(f"Duplicate type: {declaration.name}")
            if existing.rust_name() == declaration.rust_name():
                raise RuntimeError(f"{existing.name} and {declaration.name} have the same Rust name")
        document.declarations.append(declaration)

    while True:
        consume_whitespace()
        parse_includes()
        if lexer.is_eof():
            break

        attributes: List[str] = []
        parse_attributes(attributes)
        keyword = lexer.consume_until(lambda c: c.isspace())
        if keyword in ("struct", "enum", "variant"):
            parse_declaration(keyword, attributes)
        elif keyword == "endpoint" and not attributes:
            parse_endpoint()
        else:
            raise RuntimeError(f"Expected 'endpoint', 'struct', 'enum' or 'variant', got '{keyword}'")

    for endpoint in endpoints:
        endpoint.validate()

    return document


def class_declaration(endpoint: Endpoint, name: str) -> str:
    export = f"{endpoint.export_macro} " if endpoint.export_macro else ""
    return f"class {export}{name}"


def write_method(
    header: TextIO,
    implementation: TextIO,
    class_name: str,
    signature: str,
    body: str = "",
    initializers: str = "",
) -> None:
    """Write a generated member's declaration and matching out-of-line definition."""
    header.write(f"\n    {signature};\n")
    definition = signature.removeprefix("virtual ").removeprefix("static ").removeprefix("NEVER_INLINE ")
    definition = definition.removesuffix(" override")
    result_and_name, parameters = definition.split("(", 1)
    if " " in result_and_name:
        result, name = result_and_name.rsplit(" ", 1)
        result += " "
    else:
        result, name = "", result_and_name
    implementation.write(f"\n{result}{class_name}::{name}({parameters}\n{initializers}{{\n")
    implementation.write(textwrap.indent(textwrap.dedent(body).strip("\n"), "    "))
    if body.strip():
        implementation.write("\n")
    implementation.write("}\n")


def write_message_ids_enum(out: TextIO, endpoint: Endpoint) -> None:
    out.write("\nenum class MessageID : i32 {\n")
    message_id = 0

    for message in endpoint.messages:
        message_id += 1
        out.write(f"    {pascal_case(message.name)} = {message_id},\n")

        if message.is_synchronous:
            message_id += 1
            out.write(f"    {pascal_case(message.response_name())} = {message_id},\n")

    out.write("};\n")


def write_message_class(
    out: TextIO,
    implementation: TextIO,
    endpoint: Endpoint,
    name: str,
    parameters: List[Parameter],
    response_type: str = "",
) -> None:
    pascal_name = pascal_case(name)
    qualified_name = pascal_name
    out.write(f"\n{class_declaration(endpoint, pascal_name)} final : public IPC::Message {{\npublic:")
    if response_type:
        out.write(f"\n    typedef class {response_type} ResponseType;\n")
    if parameters:
        params = ", ".join(f"{p.type} {p.name}" for p in parameters)
        initializers = "    : " + "\n    , ".join(f"m_{p.name}(move({p.name}))" for p in parameters) + "\n"
        write_method(out, implementation, qualified_name, f"{pascal_name}({params})", initializers=initializers)
    else:
        out.write(f"\n    {pascal_name}() = default;\n")
    out.write(f"""
    {pascal_name}({pascal_name} const&) = default;
    {pascal_name}({pascal_name}&&) = default;
    {pascal_name}& operator=({pascal_name} const&) = default;
""")
    if len(parameters) == 1:
        out.write(f"""
    template<typename WrappedReturnType>
    requires(!SameAs<WrappedReturnType, {parameters[0].type}>)
    {pascal_name}(WrappedReturnType&& value)
        : m_{parameters[0].name}(forward<WrappedReturnType>(value))
    {{
    }}
""")
    out.write(f"\n    static constexpr u32 ENDPOINT_MAGIC = {endpoint.magic};\n")
    write_method(
        out, implementation, qualified_name, "virtual u32 endpoint_magic() const override", "return ENDPOINT_MAGIC;"
    )
    write_method(
        out,
        implementation,
        qualified_name,
        "virtual i32 message_id() const override",
        f"return (int)MessageID::{pascal_name};",
    )
    out.write(f"    static i32 static_message_id() {{ return (int)MessageID::{pascal_name}; }}\n")
    write_method(
        out,
        implementation,
        qualified_name,
        "virtual StringView message_name() const override",
        f'return "{endpoint.name}::{pascal_name}"sv;',
    )
    body = io.StringIO()
    body.write("IPC::Decoder decoder { stream, attachments };\n")
    for parameter in parameters:
        body.write(f"auto {parameter.name} = TRY((decoder.decode<{parameter.type}>()));\n")
        if "UTF8" in parameter.attributes:
            body.write(f"""if (!Utf8View({parameter.name}).validate())
    return Error::from_string_literal("Decoded {parameter.name} is invalid UTF-8");
""")
    constructor_args = ", ".join(f"move({p.name})" for p in parameters)
    body.write(f"return make<{pascal_name}>({constructor_args});\n")
    write_method(
        out,
        implementation,
        qualified_name,
        f"static ErrorOr<NonnullOwnPtr<{pascal_name}>> decode(AK::FixedMemoryStream& stream, Queue<IPC::Attachment>& attachments)",
        body.getvalue(),
    )
    encode_params = ", ".join(f"{make_argument_type(p.type_for_encoding)} {p.name}" for p in parameters)
    body = io.StringIO()
    body.write(f"""IPC::MessageBuffer buffer;
IPC::Encoder stream(buffer);
TRY(stream.encode(ENDPOINT_MAGIC));
TRY(stream.encode((int)MessageID::{pascal_name}));
""")
    for parameter in parameters:
        body.write(f"TRY(stream.encode({parameter.name}));\n")
    body.write("return buffer;\n")
    write_method(
        out,
        implementation,
        qualified_name,
        f"static ErrorOr<IPC::MessageBuffer> static_encode({encode_params})",
        body.getvalue(),
    )
    member_args = ", ".join(f"m_{p.name}" for p in parameters)
    write_method(
        out,
        implementation,
        qualified_name,
        "virtual ErrorOr<IPC::MessageBuffer> encode() const override",
        f"return static_encode({member_args});",
    )
    for parameter in parameters:
        if is_primitive_or_simple_type(parameter.type):
            out.write(f"\n    {parameter.type} {parameter.name}() const {{ return m_{parameter.name}; }}\n")
        else:
            out.write(f"\n    {parameter.type} const& {parameter.name}() const {{ return m_{parameter.name}; }}\n")
            write_method(
                out,
                implementation,
                qualified_name,
                f"{parameter.type} take_{parameter.name}()",
                f"return move(m_{parameter.name});",
            )
    out.write("\nprivate:")
    for parameter in parameters:
        out.write(f"\n    {parameter.type} m_{parameter.name};")
    out.write("\n};\n")


def write_proxy_method(
    out: TextIO,
    implementation: TextIO,
    endpoint: Endpoint,
    message: Message,
    parameters: List[Parameter],
    is_synchronous: bool,
    is_try: bool,
    is_unicode_string_overload: bool = False,
) -> None:
    # FIXME: For String parameters, we want to retain the property that all tranferred String objects are strictly UTF-8.
    #        So instead of generating a single proxy method that accepts StringView parameters, we generate two overloads.
    #        The first accepts StringView parameters, but validates the view is UTF-8. The second accepts String parameters,
    #        for callers that already have a UTF-8 String object.
    #
    #        Ideally, we will eventually have separate StringView types for each of String and ByteString, where String's
    #        view internally provides UTF-8 guarantees. Then we won't need these overloads.
    generate_unicode_string_overload = False

    return_type = "void"
    if is_synchronous:
        if len(message.outputs) == 1:
            return_type = message.outputs[0].type
        elif message.outputs:
            return_type = message_name_qualified(endpoint.name, message.name, True)

    inner_return_type = return_type
    if is_try:
        return_type = f"IPC::IPCErrorOr<{return_type}>"

    pascal_name = pascal_case(message.name)
    method_name = ("try_" if is_try else "") + ("" if is_synchronous else "async_") + message.name

    signature_params: List[str] = []
    for parameter in parameters:
        if is_synchronous or is_try:
            type = parameter.type
        elif is_unicode_string_overload:
            type = make_argument_type(parameter.type)
        else:
            type = make_argument_type(parameter.type_for_encoding)

        signature_params.append(f"{type} {parameter.name}")

    signature = f"{return_type} {method_name}({', '.join(signature_params)})"
    body = io.StringIO()

    if not is_synchronous and not is_try and not is_unicode_string_overload:
        for parameter in parameters:
            if parameter.type == "String" and parameter.type_for_encoding == "StringView":
                body.write(f"\n        VERIFY(Utf8View {{ {parameter.name} }}.validate());")
                generate_unicode_string_overload = True
            elif parameter.type == "Utf16String" and parameter.type_for_encoding == "Utf16View":
                body.write(f"\n        VERIFY({parameter.name}.validate());")
                generate_unicode_string_overload = True

    call_args_parts: List[str] = []
    for parameter in parameters:
        type = parameter.type if (is_synchronous or is_try) else parameter.type_for_encoding
        if is_primitive_or_simple_type(type):
            call_args_parts.append(parameter.name)
        else:
            call_args_parts.append(f"move({parameter.name})")
    call_args = ", ".join(call_args_parts)

    if is_synchronous and not is_try:
        sync_call = f"m_connection.send_sync<Messages::{endpoint.name}::{pascal_name}>({call_args})"

        if return_type == "void":
            body.write(f"\n        (void){sync_call};")
        elif len(message.outputs) == 1:
            output = message.outputs[0]
            accessor = output.name if is_primitive_or_simple_type(output.type) else f"take_{output.name}"
            body.write(f"\n        return {sync_call}->{accessor}();")
        else:
            body.write(f"\n        return move(*{sync_call});")
    elif is_try:
        if inner_return_type == "void":
            success_return = "{}"
        elif len(message.outputs) == 1:
            output = message.outputs[0]
            accessor = output.name if is_primitive_or_simple_type(output.type) else f"take_{output.name}"
            success_return = f"result->{accessor}()"
        else:
            success_return = "move(*result)"
        body.write(f"""
        if (auto result = m_connection.send_sync_but_allow_failure<Messages::{endpoint.name}::{pascal_name}>({call_args}))
            return {success_return};
        m_connection.shutdown();
        return IPC::ErrorCode::PeerDisconnected;""")
    else:
        # Async messages silently ignore send failures (e.g. peer disconnected).
        body.write(f"""
        auto message_buffer = MUST(Messages::{endpoint.name}::{pascal_name}::static_encode({call_args}));
        (void)m_connection.post_message(message_buffer);""")

    write_method(out, implementation, f"{endpoint.name}Proxy", signature, body.getvalue())

    if generate_unicode_string_overload:
        write_proxy_method(
            out,
            implementation,
            endpoint,
            message,
            message.inputs,
            is_synchronous,
            is_try,
            is_unicode_string_overload=True,
        )


def write_proxy_class(out: TextIO, implementation: TextIO, endpoint: Endpoint) -> None:
    name = f"{endpoint.name}Proxy"
    connection_parameter = "IPC::ConnectionBase& connection" if endpoint.messages else "IPC::ConnectionBase&"
    initializer = "        : m_connection(connection)\n" if endpoint.messages else ""
    out.write(f"""
{class_declaration(endpoint, name)} {{
public:
    // Used to disambiguate the constructor call.
    struct Tag {{ }};

    {name}({connection_parameter}, Tag)
{initializer}    {{
    }}
""")
    for message in endpoint.messages:
        write_proxy_method(out, implementation, endpoint, message, message.inputs, message.is_synchronous, is_try=False)
        if message.is_synchronous:
            write_proxy_method(
                out, implementation, endpoint, message, message.inputs, is_synchronous=False, is_try=False
            )
            write_proxy_method(out, implementation, endpoint, message, message.inputs, is_synchronous=True, is_try=True)
    if endpoint.messages:
        out.write("\nprivate:\n    IPC::ConnectionBase& m_connection;\n")
    out.write("};\n")


def write_endpoint_class(out: TextIO, implementation: TextIO, endpoint: Endpoint) -> None:
    out.write(f"""
{class_declaration(endpoint, f"{endpoint.name}Stub")};

{class_declaration(endpoint, f"{endpoint.name}Endpoint")} {{
public:
    using Proxy = {endpoint.name}Proxy;
    using Stub = {endpoint.name}Stub;

    static u32 static_magic() {{ return {endpoint.magic}; }}
""")
    body = io.StringIO()
    body.write("""
        FixedMemoryStream stream { buffer };
        auto message_endpoint_magic = TRY(stream.read_value<u32>());

        if (message_endpoint_magic != static_magic())
            return Error::from_string_literal("Endpoint magic number mismatch, not my message!");

        auto message_id = TRY(stream.read_value<i32>());

        switch (message_id) {""")

    for message in endpoint.messages:
        names = [message.name]
        if message.is_synchronous:
            names.append(message.response_name())

        for name in names:
            pascal_name = pascal_case(name)
            body.write(f"""
        case (int)Messages::{endpoint.name}::MessageID::{pascal_name}:
            return Messages::{endpoint.name}::{pascal_name}::decode(stream, attachments);""")

    body.write(f"""
        default:
            return Error::from_string_literal("Failed to decode {endpoint.name} message");
        }}

        VERIFY_NOT_REACHED();
""")
    write_method(
        out,
        implementation,
        f"{endpoint.name}Endpoint",
        "static ErrorOr<NonnullOwnPtr<IPC::Message>> decode_message(ReadonlyBytes buffer, [[maybe_unused]] Queue<IPC::Attachment>& attachments)",
        body.getvalue(),
    )
    out.write("};\n")


def write_stub_class(out: TextIO, implementation: TextIO, endpoint: Endpoint) -> None:
    # The receiver answers for every argument type its endpoint carries. Which of those types ask
    # anything of it is decided by the types themselves.
    argument_types: List[str] = []
    for message in endpoint.messages:
        for parameter in message.inputs:
            if parameter.type not in argument_types:
                argument_types.append(parameter.type)

    bases = "public IPC::Stub"
    if argument_types:
        bases += f"\n    , public IPC::SenderClaimReceivers<{', '.join(argument_types)}>"

    out.write(f"""
{class_declaration(endpoint, f"{endpoint.name}Stub")} : {bases} {{
public:
    {endpoint.name}Stub() {{ }}
    virtual ~{endpoint.name}Stub() override {{ }}

    virtual u32 magic() const override {{ return {endpoint.magic}; }}
    virtual ByteString name() const override {{ return "{endpoint.name}"; }}

""")
    body = io.StringIO()
    body.write("\n        switch (message->message_id()) {\n")

    for message in endpoint.messages:
        pascal_name = pascal_case(message.name)
        body.write(f"\n        case (int)Messages::{endpoint.name}::MessageID::{pascal_name}:\n")

        if message.inputs:
            body.write(f"            return handle_{message.name}(*message);")
        else:
            body.write(f"            return handle_{message.name}();")

    body.write(f"""
        default:
            return Error::from_string_literal("Unknown message ID for {endpoint.name} endpoint");
        }}
""")
    write_method(
        out,
        implementation,
        f"{endpoint.name}Stub",
        "virtual ErrorOr<OwnPtr<IPC::MessageBuffer>> handle(NonnullOwnPtr<IPC::Message> message) override",
        body.getvalue(),
    )

    for message in endpoint.messages:
        pascal_name = pascal_case(message.name)

        arg_parts: List[str] = []
        for parameter in message.inputs:
            accessor = parameter.name if is_primitive_or_simple_type(parameter.type) else f"take_{parameter.name}"
            arg_parts.append(f"request.{accessor}()")
        arguments = ", ".join(arg_parts)

        body = io.StringIO()
        signature = f"NEVER_INLINE ErrorOr<OwnPtr<IPC::MessageBuffer>> handle_{message.name}("

        if message.inputs:
            signature += "IPC::Message& message)"
            body.write(f"        auto& request = static_cast<Messages::{endpoint.name}::{pascal_name}&>(message);\n")
        else:
            signature += ")"

        # An argument whose type is a claim by the sender about itself is verified before the handler
        # runs. Which types make a claim is decided by the types, not here.
        for parameter in message.inputs:
            reason = f"argument '{parameter.name}' is not a valid claim for this sender"
            body.write(f"        if (!IPC::verify_message_argument(*this, request.{parameter.name}())) {{\n")
            body.write(f'            did_misbehave("{message.name}"sv, "{reason}"sv);\n')

            if not message.is_synchronous:
                body.write("            return nullptr;\n")
            else:
                refused_types = [message_name_qualified(endpoint.name, message.name, True)]
                refused_types += [output.type for output in message.outputs]
                body.write(f"            return IPC::refused_reply<{', '.join(refused_types)}>();\n")

            body.write("        }\n")

        if not message.is_synchronous:
            body.write(f"        {message.name}({arguments});\n")
            body.write("        return nullptr;\n")
        elif not message.outputs:
            response_pascal_name = pascal_case(message.response_name())
            body.write(f"        {message.name}({arguments});\n")
            body.write(f"        auto response = Messages::{endpoint.name}::{response_pascal_name} {{}};\n")
            body.write("        return make<IPC::MessageBuffer>(TRY(response.encode()));\n")
        else:
            body.write(f"        auto response = {message.name}({arguments});\n")
            body.write("        return make<IPC::MessageBuffer>(TRY(response.encode()));\n")

        write_method(out, implementation, f"{endpoint.name}Stub", signature, body.getvalue())

    out.write("\n")

    for message in endpoint.messages:
        return_type = "void"

        if message.is_synchronous and message.outputs:
            return_type = message_name_qualified(endpoint.name, message.name, True)

        params = ", ".join(f"{p.type} {p.name}" for p in message.inputs)
        out.write(f"    virtual {return_type} {message.name}({params}) = 0;\n")

    out.write("};\n")


def write_routed_stub_class(out: TextIO, endpoint: Endpoint) -> None:
    assert endpoint.route is not None
    scope = endpoint.route_scope_pascal()

    out.write(f"""
// The receiver of the messages about one {endpoint.route_scope()}: those whose first argument is the {endpoint.route.name}.
{class_declaration(endpoint, f"{endpoint.name}{scope}Stub")} {{
public:
    virtual ~{endpoint.name}{scope}Stub() = default;

""")

    for message in endpoint.messages:
        if not endpoint.routes(message):
            continue
        return_type = "void"
        if message.is_synchronous and message.outputs:
            return_type = message_name_qualified(endpoint.name, message.name, True)
        params = ", ".join(f"{p.type} {p.name}" for p in message.inputs[1:])
        out.write(f"    virtual {return_type} {message.name}({params}) = 0;\n")

    out.write("};\n")


def write_routing_stub_class(out: TextIO, implementation: TextIO, endpoint: Endpoint) -> None:
    assert endpoint.route is not None
    scope = endpoint.route_scope_pascal()
    route = endpoint.route
    stub_accessor = f"{endpoint.route_scope()}_stub"

    out.write(f"""
// A receiver that hands each message about a {endpoint.route_scope()} to that {endpoint.route_scope()}'s stub, and drops
// it when there is none. It still answers the unrouted messages itself, and may take a routed one back by
// overriding it.
{class_declaration(endpoint, f"{endpoint.name}{scope}RoutingStub")} : public {endpoint.name}Stub {{
public:
    virtual {endpoint.name}{scope}Stub* {stub_accessor}({route.type} const&) = 0;
""")

    for message in endpoint.messages:
        if not endpoint.routes(message):
            continue
        params = ", ".join(f"{p.type} {p.name}" for p in message.inputs)
        args = ", ".join(
            p.name if is_primitive_or_simple_type(p.type) else f"move({p.name})" for p in message.inputs[1:]
        )
        write_method(
            out,
            implementation,
            f"{endpoint.name}{scope}RoutingStub",
            f"virtual void {message.name}({params}) override",
            f"if (auto* stub = {stub_accessor}({route.name}))\n    stub->{message.name}({args});",
        )

    out.write("};\n")


def write_bound_proxy_method(
    out: TextIO,
    implementation: TextIO,
    endpoint: Endpoint,
    message: Message,
    is_synchronous: bool,
    is_try: bool,
    is_unicode_string_overload: bool = False,
) -> None:
    assert endpoint.route is not None
    parameters = message.inputs[1:]

    return_type = "void"
    if is_synchronous:
        if len(message.outputs) == 1:
            return_type = message.outputs[0].type
        elif message.outputs:
            return_type = message_name_qualified(endpoint.name, message.name, True)
    if is_try:
        return_type = f"IPC::IPCErrorOr<{return_type}>"

    method_name = ("try_" if is_try else "") + ("" if is_synchronous else "async_") + message.name

    signature_params: List[str] = []
    call_args: List[str] = [f"routed_{endpoint.route.name}()"]
    generate_unicode_string_overload = False
    for parameter in parameters:
        if is_synchronous or is_try:
            type = parameter.type
        elif is_unicode_string_overload:
            type = make_argument_type(parameter.type)
        else:
            type = make_argument_type(parameter.type_for_encoding)
            if parameter.type == "String" and parameter.type_for_encoding == "StringView":
                generate_unicode_string_overload = True
            elif parameter.type == "Utf16String" and parameter.type_for_encoding == "Utf16View":
                generate_unicode_string_overload = True
        signature_params.append(f"{type} {parameter.name}")

        forwarded_type = (
            parameter.type if (is_synchronous or is_try or is_unicode_string_overload) else parameter.type_for_encoding
        )
        call_args.append(parameter.name if is_primitive_or_simple_type(forwarded_type) else f"move({parameter.name})")

    call = f"connection->{method_name}({', '.join(call_args)})"
    signature = f"{return_type} {method_name}({', '.join(signature_params)}) const"
    body = io.StringIO()
    body.write("        if (auto* connection = routed_proxy())\n")
    if return_type == "void":
        body.write(f"            {call};\n")
    else:
        body.write(f"            return {call};\n")
        if is_try:
            body.write("        return IPC::ErrorCode::PeerDisconnected;\n")
        elif len(message.outputs) == 1:
            body.write(f"        return IPC::empty_value<{return_type}>();\n")
        else:
            outputs = ", ".join(output.type for output in message.outputs)
            body.write(f"        return IPC::empty_response<{return_type}, {outputs}>();\n")
    write_method(
        out, implementation, f"{endpoint.name}{endpoint.route_scope_pascal()}Proxy", signature, body.getvalue()
    )

    if generate_unicode_string_overload:
        write_bound_proxy_method(
            out, implementation, endpoint, message, is_synchronous, is_try, is_unicode_string_overload=True
        )


def write_bound_proxy_class(out: TextIO, implementation: TextIO, endpoint: Endpoint) -> None:
    assert endpoint.route is not None
    scope = endpoint.route_scope_pascal()

    out.write(f"""
// Sends the messages about one {endpoint.route_scope()} without naming it each time. The class deriving this provides
// routed_proxy(), the proxy the messages go through, or null once the {endpoint.route_scope()} has none, and
// routed_{endpoint.route.name}(). A message for a {endpoint.route_scope()} without a connection is dropped.
{class_declaration(endpoint, f"{endpoint.name}{scope}Proxy")} {{
public:
    virtual ~{endpoint.name}{scope}Proxy() = default;
""")

    for message in endpoint.messages:
        if not endpoint.binds(message):
            continue
        write_bound_proxy_method(out, implementation, endpoint, message, message.is_synchronous, is_try=False)
        if message.is_synchronous:
            write_bound_proxy_method(out, implementation, endpoint, message, is_synchronous=False, is_try=False)
            write_bound_proxy_method(out, implementation, endpoint, message, is_synchronous=True, is_try=True)

    out.write(f"""
protected:
    virtual {endpoint.name}Proxy* routed_proxy() const = 0;
    virtual {endpoint.route.type} routed_{endpoint.route.name}() const = 0;
}};
""")


def write_endpoint(out: TextIO, implementation: TextIO, endpoint: Endpoint) -> None:
    out.write(f"\nnamespace Messages::{endpoint.name} {{\n")
    implementation.write(f"\nnamespace Messages::{endpoint.name} {{\n")
    write_message_ids_enum(out, endpoint)
    for message in endpoint.messages:
        response_name = ""
        if message.is_synchronous:
            response_name = message.response_name()
            write_message_class(out, implementation, endpoint, response_name, message.outputs)
        write_message_class(out, implementation, endpoint, message.name, message.inputs, response_name)
    out.write(f"\n}} // namespace Messages::{endpoint.name}\n")
    implementation.write(f"\n}} // namespace Messages::{endpoint.name}\n")
    write_proxy_class(out, implementation, endpoint)
    write_endpoint_class(out, implementation, endpoint)
    write_stub_class(out, implementation, endpoint)
    if endpoint.route is not None:
        write_routed_stub_class(out, endpoint)
        write_routing_stub_class(out, implementation, endpoint)
        write_bound_proxy_class(out, implementation, endpoint)


def cpp_field_value(declaration: Declaration, member: Parameter) -> str:
    if declaration.has_accessors():
        return f"value.{member.name}()"
    return f"value.{member.name}"


def max_size(member: Parameter) -> Optional[str]:
    """The MaxBytes or MaxCount of a field, which bounds the size that its string, byte buffer or vector goes with."""
    for name, argument in field_attributes(member):
        if name in ("MaxBytes", "MaxCount"):
            return argument
    return None


def cpp_field_decode(member: Parameter) -> str:
    size = max_size(member)
    if size is not None:
        return f"TRY((decoder.decode_with_max_size<{member.type}>({size})))"
    return f"TRY((decoder.decode<{member.type}>()))"


def cpp_field_refusals(declaration: Declaration, member: Parameter) -> List[FieldRefusal]:
    return field_refusals(member, cpp_field_value(declaration, member), "")


def write_cpp_definition(out: TextIO, declaration: Declaration) -> None:
    name = declaration.name.rpartition("::")[2]
    out.write("\n")
    if declaration.kind == "enum":
        out.write(f"enum class {name} : {declaration.underlying_type} {{\n")
        for member in declaration.members:
            out.write(f"    {member.name},\n")
        out.write("};\n")
    elif declaration.kind == "struct" and not declaration.members:
        out.write(f"struct {name} {{ }};\n")
    elif declaration.kind == "struct":
        out.write(f"struct {name} {{\n")
        for member in declaration.members:
            out.write(f"    {member.type} {member.name} {{}};\n")
        out.write("};\n")
    else:
        alternatives = ", ".join(member.type for member in declaration.members)
        out.write(f"using {name} = Variant<{alternatives}>;\n")


def write_cpp_enum_codec(out: TextIO, declaration: Declaration) -> None:
    name = declaration.name
    underlying_type = declaration.underlying_type

    # NB: An enum goes as the position of its value in the declaration, which C++ need not number the same way.
    out.write(f"""
template<>
ErrorOr<void> encode(Encoder& encoder, {name} const& value)
{{
    switch (value) {{
""")
    for index, member in enumerate(declaration.members):
        out.write(f"    case {name}::{member.name}:\n        return encoder.encode<{underlying_type}>({index});\n")
    out.write(f"""    }}
    return Error::from_string_literal("IPC encode: Invalid {name}");
}}

template<>
ErrorOr<{name}> decode(Decoder& decoder)
{{
    switch (TRY(decoder.decode<{underlying_type}>())) {{
""")
    for index, member in enumerate(declaration.members):
        out.write(f"    case {index}:\n        return {name}::{member.name};\n")
    out.write(f"""    default:
        return Error::from_string_literal("IPC decode: Invalid {name}");
    }}
}}
""")


def write_cpp_struct_codec(out: TextIO, declaration: Declaration) -> None:
    name = declaration.name
    if not declaration.members:
        out.write(f"""
template<>
ErrorOr<void> encode(Encoder&, {name} const&)
{{
    return {{}};
}}

template<>
ErrorOr<{name}> decode(Decoder&)
{{
    return {name} {{}};
}}
""")
        return

    # NB: Each field must have the type it is declared with, which is the type the decoder reads.
    out.write(f"\ntemplate<>\nErrorOr<void> encode(Encoder& encoder, {name} const& value)\n{{\n")
    for member in declaration.members:
        field_type = f"decltype({cpp_field_value(declaration, member)})"
        if declaration.has_accessors():
            field_type = f"RemoveCVReference<{field_type}>"
        out.write(f"    static_assert(IsSame<{field_type}, {member.type}>);\n")
    for member in declaration.members:
        for refusal in cpp_field_refusals(declaration, member):
            out.write(f"    if ({refusal.cpp_condition})\n")
            out.write(
                f'        return Error::from_string_literal("IPC encode: {name}::{member.name} {refusal.meaning}");\n'
            )
    for member in declaration.members:
        out.write(f"    TRY(encoder.encode({cpp_field_value(declaration, member)}));\n")
    out.write("    return {};\n}\n")

    checks = [
        (member, refusal)
        for member in declaration.members
        for refusal in cpp_field_refusals(declaration, member)
        if refusal.is_checked_after_decoding()
    ]
    out.write(f"\ntemplate<>\nErrorOr<{name}> decode(Decoder& decoder)\n{{\n")
    out.write(f"    auto value = {name} {{\n" if checks else f"    return {name} {{\n")
    for member in declaration.members:
        designator = "" if declaration.has_accessors() else f".{member.name} = "
        out.write(f"        {designator}{cpp_field_decode(member)},\n")
    out.write("    };\n")
    for member, refusal in checks:
        out.write(f"    if ({refusal.cpp_condition})\n")
        out.write(
            f'        return Error::from_string_literal("IPC decode: {name}::{member.name} {refusal.meaning}");\n'
        )
    if checks:
        out.write("    return value;\n")
    out.write("}\n")


def write_cpp_declarations(header: TextIO, implementation: TextIO, document: Document, export_macro: str) -> None:
    namespace = ""
    for declaration in document.declarations:
        if declaration.is_existing_cpp():
            continue
        declaration_namespace = declaration.name.rpartition("::")[0]
        if declaration_namespace != namespace:
            if namespace:
                header.write(f"\n}} // namespace {namespace}\n")
            header.write(f"\nnamespace {declaration_namespace} {{\n")
            namespace = declaration_namespace
        write_cpp_definition(header, declaration)
    if namespace:
        header.write(f"\n}} // namespace {namespace}\n")

    # NB: A variant goes as the AK::Variant it is, which LibIPC already knows how to encode.
    codec_declarations = [declaration for declaration in document.declarations if declaration.kind != "variant"]
    if not codec_declarations:
        return

    export = f"{export_macro} " if export_macro else ""
    header.write("\nnamespace IPC {\n")
    implementation.write("\nnamespace IPC {\n")
    for declaration in codec_declarations:
        header.write(f"""
template<>
{export}ErrorOr<void> encode(Encoder&, {declaration.name} const&);

template<>
{export}ErrorOr<{declaration.name}> decode(Decoder&);
""")
        if declaration.kind == "enum":
            write_cpp_enum_codec(implementation, declaration)
        else:
            write_cpp_struct_codec(implementation, declaration)
    header.write("\n} // namespace IPC\n")
    implementation.write("\n} // namespace IPC\n")


def build(
    out: TextIO,
    document: Document,
    implementation: TextIO,
    header_name: str,
    library: str = "",
    export_macro: str = "",
) -> None:
    """Writes the C++ code of an IPC file. The export macro is that of the library's Export.h, if it has one."""
    out.write("#pragma once\n\n")
    endpoint_export_macro = ""
    if document.endpoints and library:
        endpoint_export_macro = f"{library.removeprefix('Lib').upper()}_API"
        out.write(f"""\
#if __has_include(<{library}/Export.h>)
#include <{library}/Export.h>
#else
#define {endpoint_export_macro}
#endif

""")
    elif export_macro:
        out.write(f"#include <{library}/Export.h>\n")
    for include in document.includes:
        out.write(f"{include}\n")
    # Message and struct members need complete owning types even when the file has no explicit includes.
    defined_declarations = [declaration for declaration in document.declarations if not declaration.is_existing_cpp()]
    member_types = " ".join(
        [
            parameter.type
            for endpoint in document.endpoints
            for message in endpoint.messages
            for parameter in message.inputs + message.outputs
        ]
        + [member.type for declaration in defined_declarations for member in declaration.members]
    )
    names = ["ByteBuffer", "ByteString", "JsonValue", "Optional", "String", "Utf16String", "Vector"]
    names = [name for name in names if re.search(rf"\b{name}\b", member_types)]
    # NB: A struct defined here holds any AK type its fields name, such as a HashMap.
    for declaration in defined_declarations:
        for member in declaration.members:
            for name in re.findall(r"(?<![\w:])[A-Z]\w*(?![\w:])", member.type):
                if name not in names and (AK_DIRECTORY / f"{name}.h").exists():
                    names.append(name)
    if any(declaration.kind == "variant" for declaration in defined_declarations) and "Variant" not in names:
        names.append("Variant")
    for name in sorted(names):
        out.write(f"#include <AK/{name}.h>\n")

    implementation.write(f'#include "{header_name}"\n\n')
    if document.endpoints:
        out.write("""\
#include <AK/Error.h>
#include <AK/OwnPtr.h>
#include <AK/Platform.h>
#include <AK/Result.h>
#include <LibIPC/File.h>
#include <LibIPC/Message.h>
#include <LibIPC/SenderClaim.h>
#include <LibIPC/Stub.h>

namespace AK {
class FixedMemoryStream;
}

namespace IPC {
class ConnectionBase;
}

#if defined(AK_COMPILER_CLANG)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdefaulted-function-deleted"
#endif
""")
        implementation.write("""\
#include <AK/MemoryStream.h>
#include <AK/Utf8View.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Connection.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
""")
    else:
        out.write("#include <LibIPC/Forward.h>\n")
        implementation.write("#include <LibIPC/Decoder.h>\n#include <LibIPC/Encoder.h>\n")
    if any(
        name == "Finite"
        for declaration in document.declarations
        for member in declaration.members
        for name, _ in field_attributes(member)
    ):
        implementation.write("#include <math.h>\n")

    write_cpp_declarations(out, implementation, document, export_macro)
    for endpoint in document.endpoints:
        endpoint.export_macro = endpoint_export_macro
        write_endpoint(out, implementation, endpoint)
    if document.endpoints:
        out.write("""
#if defined(AK_COMPILER_CLANG)
#pragma clang diagnostic pop
#endif
""")


RUST_BUILTIN_TYPES = {
    "bool": "bool",
    "u8": "u8",
    "u16": "u16",
    "u32": "u32",
    "u64": "u64",
    "i8": "i8",
    "i16": "i16",
    "i32": "i32",
    "i64": "i64",
    "int": "i32",
    "unsigned": "u32",
    "size_t": "usize",
    "float": "f32",
    "double": "f64",
    "String": "::libipc_rust::wire::Utf8String",
    "ByteString": "::libipc_rust::wire::ByteString",
    "Utf16String": "::libipc_rust::wire::Utf16String",
    "ByteBuffer": "::std::vec::Vec<u8>",
    "Empty": "()",
    "IPC::File": "::libipc_rust::wire::File",
}

RUST_KEYWORDS = {
    "Self", "abstract", "as", "async", "await", "become", "box", "break", "const", "continue", "crate", "do", "dyn",
    "else", "enum", "extern", "false", "final", "fn", "for", "gen", "if", "impl", "in", "let", "loop", "macro",
    "macro_rules", "match", "mod", "move", "mut", "override", "priv", "pub", "raw", "ref", "return", "safe", "self",
    "static", "struct", "super", "trait", "true", "try", "type", "typeof", "union", "unsafe", "unsized", "use",
    "virtual", "where", "while", "yield",
}  # fmt: skip


def split_template_arguments(arguments: str) -> List[str]:
    parts: List[str] = []
    depth = 0
    current = ""
    for character in arguments:
        if character == "<":
            depth += 1
        elif character == ">":
            depth -= 1
        if character == "," and depth == 0:
            parts.append(current.strip())
            current = ""
            continue
        current += character
    if current.strip():
        parts.append(current.strip())
    return parts


def rust_type(cpp_type: str) -> str:
    cpp_type = cpp_type.strip()
    if cpp_type.startswith("::"):
        cpp_type = cpp_type[2:]
    if cpp_type in RUST_BUILTIN_TYPES:
        return RUST_BUILTIN_TYPES[cpp_type]
    if "<" in cpp_type and cpp_type.endswith(">"):
        template = cpp_type[: cpp_type.index("<")]
        arguments = split_template_arguments(cpp_type[cpp_type.index("<") + 1 : -1])
        if template == "Vector":
            return f"::std::vec::Vec<{rust_type(arguments[0])}>"
        if template == "Optional":
            return f"::std::option::Option<{rust_type(arguments[0])}>"
        if template == "Array" and len(arguments) == 2 and arguments[1].isdecimal():
            return f"[{rust_type(arguments[0])}; {arguments[1]}]"
        raise RuntimeError(f"No Rust type for {cpp_type}")
    # NB: Namespaced user types are supplied by the crate, with "::" as "_".
    if re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*(?:::[A-Za-z_][A-Za-z_0-9]*)+", cpp_type):
        return "crate::ipc_types::" + rust_field_name(cpp_type.replace("::", "_"))
    raise RuntimeError(f"No Rust type for {cpp_type}")


def rust_field_name(name: str) -> str:
    if name in {"self", "super", "crate", "Self", "_"}:
        raise RuntimeError(f"Cannot use {name!r} as a Rust identifier")
    return f"r#{name}" if name in RUST_KEYWORDS else name


def snake_case(identifier: str) -> str:
    result = ""
    for index, character in enumerate(identifier):
        if character.isupper() and index > 0 and not identifier[index - 1].isupper():
            result += "_"
        result += character.lower()
    return result


def rust_utf8_bytes(parameter: Parameter, value: str) -> Optional[str]:
    field_type = rust_type(parameter.type)
    if field_type == RUST_BUILTIN_TYPES["String"]:
        return None
    if field_type == RUST_BUILTIN_TYPES["ByteString"]:
        return f"&{value}.0"
    if field_type == RUST_BUILTIN_TYPES["ByteBuffer"]:
        return f"&{value}"
    raise RuntimeError(f"Cannot apply [UTF8] to Rust parameter {parameter.name!r} of type {parameter.type}")


def write_rust_message_struct(out: TextIO, name: str, message_id: int, parameters: List[Parameter]) -> None:
    pascal_name = rust_field_name(pascal_case(name))
    utf8_parameters = [
        parameter
        for parameter in parameters
        if "UTF8" in parameter.attributes and rust_utf8_bytes(parameter, "value") is not None
    ]
    out.write("\n")
    if parameters:
        out.write(f"    pub struct {pascal_name} {{\n")
        for parameter in parameters:
            field_type = rust_type(parameter.type)
            out.write(f"        pub {rust_field_name(parameter.name)}: {field_type},\n")
        out.write("    }\n")
    else:
        out.write(f"    pub struct {pascal_name};\n")

    if utf8_parameters:
        out.write(f"""
    impl {pascal_name} {{
        fn validate_utf8(&self) -> ::std::result::Result<(), ::libipc_rust::wire::EncodeError> {{
""")
        for parameter in utf8_parameters:
            bytes_expression = rust_utf8_bytes(parameter, f"self.{rust_field_name(parameter.name)}")
            out.write(
                f'            ::libipc_rust::wire::validate_utf8({bytes_expression}).map_err(|_| ::libipc_rust::wire::EncodeError::InvalidValue("UTF-8"))?;\n'
            )
        out.write("""            ::std::result::Result::Ok(())
        }
    }
""")

    out.write(f"""
    impl ::libipc_rust::wire::EndpointMessage for {pascal_name} {{
        const ENDPOINT_MAGIC: u32 = super::ENDPOINT_MAGIC;
        const MESSAGE_ID: i32 = {message_id};
    }}

    impl ::libipc_rust::wire::Encode for {pascal_name} {{
        fn encode(&self, _encoder: &mut ::libipc_rust::wire::Encoder) -> ::std::result::Result<(), ::libipc_rust::wire::EncodeError> {{
""")
    if utf8_parameters:
        out.write("            self.validate_utf8()?;\n")
    for parameter in parameters:
        out.write(f"            _encoder.encode(&self.{rust_field_name(parameter.name)})?;\n")
    out.write("""            ::std::result::Result::Ok(())
        }

        fn encode_owned(self, _encoder: &mut ::libipc_rust::wire::Encoder) -> ::std::result::Result<(), ::libipc_rust::wire::EncodeError> {
""")
    if utf8_parameters:
        out.write("            self.validate_utf8()?;\n")
    for parameter in parameters:
        out.write(f"            _encoder.encode_owned(self.{rust_field_name(parameter.name)})?;\n")
    out.write(f"""            ::std::result::Result::Ok(())
        }}
    }}

    impl ::libipc_rust::wire::Decode for {pascal_name} {{
        fn decode(_decoder: &mut ::libipc_rust::wire::Decoder<'_>) -> ::std::result::Result<Self, ::libipc_rust::wire::DecodeError> {{
""")
    if parameters:
        out.write("            ::std::result::Result::Ok(Self {\n")
        for parameter in parameters:
            field_name = rust_field_name(parameter.name)
            if parameter not in utf8_parameters:
                out.write(f"                {field_name}: _decoder.decode()?,\n")
                continue
            field_type = rust_type(parameter.type)
            bytes_expression = rust_utf8_bytes(parameter, "value")
            out.write(f"""                {field_name}: {{
                    let value: {field_type} = _decoder.decode()?;
                    ::libipc_rust::wire::validate_utf8({bytes_expression})?;
                    value
                }},
""")
        out.write("            })\n")
    else:
        out.write("            ::std::result::Result::Ok(Self)\n")
    out.write("        }\n    }\n")


def write_rust_endpoint(out: TextIO, endpoint: Endpoint) -> None:
    out.write(f"""
#[allow(clippy::all, dead_code)]
pub mod {rust_field_name(snake_case(endpoint.name))} {{
    pub const ENDPOINT_MAGIC: u32 = {endpoint.magic};
""")

    out.write("\n    pub mod messages {\n")
    message_output = io.StringIO()
    variants: List[str] = []
    message_id = 0
    for message in endpoint.messages:
        message_id += 1
        write_rust_message_struct(message_output, message.name, message_id, message.inputs)
        variants.append(rust_field_name(pascal_case(message.name)))
        if message.is_synchronous:
            message_id += 1
            write_rust_message_struct(message_output, message.response_name(), message_id, message.outputs)
            variants.append(rust_field_name(pascal_case(message.response_name())))

    out.write(textwrap.indent(message_output.getvalue(), "    "))
    out.write("    }\n\n    pub enum Message {\n")
    for variant in variants:
        out.write(f"        {variant}(messages::{variant}),\n")
    out.write("    }\n")
    out.write("""
    impl Message {
        /// Decodes a message of this endpoint, allowing unused bytes and attachments as C++ does.
        pub fn decode(bytes: &[u8], attachments: ::std::vec::Vec<::libipc_rust::Attachment>) -> ::std::result::Result<Self, ::libipc_rust::wire::DecodeError> {
            let mut decoder = ::libipc_rust::wire::Decoder::new(bytes, attachments);
            let (magic, id) = ::libipc_rust::wire::decode_message_header(&mut decoder)?;
            if magic != ENDPOINT_MAGIC {
                return Err(::libipc_rust::wire::DecodeError::InvalidValue("endpoint magic"));
            }
            let message = match id {
""")
    for variant in variants:
        out.write(
            f"                <messages::{variant} as ::libipc_rust::wire::EndpointMessage>::MESSAGE_ID => Self::{variant}(decoder.decode()?),\n"
        )
    out.write("""                _ => return Err(::libipc_rust::wire::DecodeError::InvalidValue("message ID")),
            };
            ::std::result::Result::Ok(message)
        }
""")
    out.write("    }\n}\n")


def rust_field_refusals(member: Parameter, value: str) -> List[FieldRefusal]:
    return field_refusals(member, "", f"{value}.{rust_field_name(member.name)}")


def write_rust_enum(out: TextIO, declaration: Declaration, name: str) -> None:
    underlying_type = declaration.underlying_type
    out.write(f"""
#[allow(dead_code, non_camel_case_types)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr({underlying_type})]
pub enum {name} {{
""")
    for member in declaration.members:
        out.write(f"    {rust_field_name(member.name)},\n")
    out.write(f"""}}

impl ::libipc_rust::wire::Encode for {name} {{
    fn encode(&self, encoder: &mut ::libipc_rust::wire::Encoder) -> ::std::result::Result<(), ::libipc_rust::wire::EncodeError> {{
        encoder.encode(&(*self as {underlying_type}))
    }}
}}

impl ::libipc_rust::wire::Decode for {name} {{
    fn decode(decoder: &mut ::libipc_rust::wire::Decoder<'_>) -> ::std::result::Result<Self, ::libipc_rust::wire::DecodeError> {{
        match decoder.decode::<{underlying_type}>()? {{
""")
    for index, member in enumerate(declaration.members):
        out.write(f"            {index} => ::std::result::Result::Ok(Self::{rust_field_name(member.name)}),\n")
    out.write(f"""            _ => ::std::result::Result::Err(::libipc_rust::wire::DecodeError::InvalidValue("{declaration.name}")),
        }}
    }}
}}
""")


def write_rust_struct(out: TextIO, declaration: Declaration, name: str) -> None:
    fields = [(rust_field_name(member.name), rust_type(member.type)) for member in declaration.members]
    out.write("\n#[allow(dead_code, non_camel_case_types)]\n")
    if not fields:
        out.write(f"""pub struct {name};

impl ::libipc_rust::wire::Encode for {name} {{
    fn encode(&self, _: &mut ::libipc_rust::wire::Encoder) -> ::std::result::Result<(), ::libipc_rust::wire::EncodeError> {{
        ::std::result::Result::Ok(())
    }}
}}

impl ::libipc_rust::wire::Decode for {name} {{
    fn decode(_: &mut ::libipc_rust::wire::Decoder<'_>) -> ::std::result::Result<Self, ::libipc_rust::wire::DecodeError> {{
        ::std::result::Result::Ok(Self)
    }}
}}
""")
        return

    out.write(f"pub struct {name} {{\n")
    for field_name, field_type in fields:
        out.write(f"    pub {field_name}: {field_type},\n")
    out.write("}\n")

    refusals = [(member, refusal) for member in declaration.members for refusal in rust_field_refusals(member, "self")]
    if refusals:
        out.write(f"""
impl {name} {{
    fn validate(&self) -> ::std::result::Result<(), ::libipc_rust::wire::EncodeError> {{
""")
        for member, refusal in refusals:
            out.write(f"""        if {refusal.rust_condition} {{
            return ::std::result::Result::Err(::libipc_rust::wire::EncodeError::InvalidValue("{declaration.name}::{member.name}"));
        }}
""")
        out.write("        ::std::result::Result::Ok(())\n    }\n}\n")
    validate = "        self.validate()?;\n" if refusals else ""

    out.write(f"""
impl ::libipc_rust::wire::Encode for {name} {{
    fn encode(&self, encoder: &mut ::libipc_rust::wire::Encoder) -> ::std::result::Result<(), ::libipc_rust::wire::EncodeError> {{
{validate}""")
    for field_name, _ in fields:
        out.write(f"        encoder.encode(&self.{field_name})?;\n")
    out.write(f"""        ::std::result::Result::Ok(())
    }}

    fn encode_owned(self, encoder: &mut ::libipc_rust::wire::Encoder) -> ::std::result::Result<(), ::libipc_rust::wire::EncodeError> {{
{validate}""")
    for field_name, _ in fields:
        out.write(f"        encoder.encode_owned(self.{field_name})?;\n")
    checks = [
        (member, refusal)
        for member in declaration.members
        for refusal in rust_field_refusals(member, "value")
        if refusal.is_checked_after_decoding()
    ]
    out.write(f"""        ::std::result::Result::Ok(())
    }}
}}

impl ::libipc_rust::wire::Decode for {name} {{
    fn decode(decoder: &mut ::libipc_rust::wire::Decoder<'_>) -> ::std::result::Result<Self, ::libipc_rust::wire::DecodeError> {{
""")
    out.write("        let value = Self {\n" if checks else "        ::std::result::Result::Ok(Self {\n")
    for member in declaration.members:
        size = max_size(member)
        decode = "decoder.decode()?" if size is None else f"decoder.decode_with_max_size({size})?"
        out.write(f"            {rust_field_name(member.name)}: {decode},\n")
    if not checks:
        out.write("        })\n    }\n}\n")
        return
    out.write("        };\n")
    for member, refusal in checks:
        out.write(f"""        if {refusal.rust_condition} {{
            return ::std::result::Result::Err(::libipc_rust::wire::DecodeError::InvalidValue("{declaration.name}::{member.name}"));
        }}
""")
    out.write("""        ::std::result::Result::Ok(value)
    }
}
""")


def write_rust_variant(out: TextIO, declaration: Declaration, name: str) -> None:
    alternatives = [(rust_field_name(member.name), rust_type(member.type)) for member in declaration.members]
    out.write(f"""
#[allow(dead_code, non_camel_case_types)]
pub enum {name} {{
""")
    for alternative, alternative_type in alternatives:
        out.write(f"    {alternative}({alternative_type}),\n")
    out.write(f"""}}

// NB: A variant goes as the index of its alternative, then the alternative, as an AK::Variant does.
impl ::libipc_rust::wire::Encode for {name} {{
""")
    for function, receiver, encode in (("encode", "&self", "encode"), ("encode_owned", "self", "encode_owned")):
        out.write(f"""    fn {function}({receiver}, encoder: &mut ::libipc_rust::wire::Encoder) -> ::std::result::Result<(), ::libipc_rust::wire::EncodeError> {{
        match self {{
""")
        for index, (alternative, _) in enumerate(alternatives):
            out.write(f"""            Self::{alternative}(value) => {{
                encoder.encode(&{index}u8)?;
                encoder.{encode}(value)
            }}
""")
        out.write("        }\n    }\n")
        if function == "encode":
            out.write("\n")
    out.write(f"""}}

impl ::libipc_rust::wire::Decode for {name} {{
    fn decode(decoder: &mut ::libipc_rust::wire::Decoder<'_>) -> ::std::result::Result<Self, ::libipc_rust::wire::DecodeError> {{
        match decoder.decode::<u8>()? {{
""")
    for index, (alternative, _) in enumerate(alternatives):
        out.write(f"            {index} => ::std::result::Result::Ok(Self::{alternative}(decoder.decode()?)),\n")
    out.write(f"""            _ => ::std::result::Result::Err(::libipc_rust::wire::DecodeError::InvalidValue("{declaration.name}")),
        }}
    }}
}}
""")


def build_rust(out: TextIO, document: Document) -> None:
    out.write("// NB: Generated by the IPC endpoint compiler.\n")
    # NB: Declared types refer to each other, and endpoints refer to them, as crate::ipc_types::Namespace_Name, so the
    #     crate includes this file in its ipc_types module.
    for declaration in document.declarations:
        name = rust_field_name(declaration.rust_name())
        if declaration.kind == "enum":
            write_rust_enum(out, declaration, name)
        elif declaration.kind == "struct":
            write_rust_struct(out, declaration, name)
        else:
            write_rust_variant(out, declaration, name)
    for endpoint in document.endpoints:
        write_rust_endpoint(out, endpoint)


def main():
    parser = argparse.ArgumentParser(description="Ladybird IPC endpoint compiler")
    parser.add_argument("--input", required=True, help="IPC endpoint definition file")
    parser.add_argument("--output", help="Generated header file")
    parser.add_argument("--implementation", help="Generated implementation file")
    parser.add_argument("--header-name", help="Header name to include from the implementation")
    parser.add_argument(
        "--library", default="", help="Owning library, e.g. LibWebCommon; determines the export header and macro"
    )
    parser.add_argument(
        "--export-macro", default="", help="Export macro of the owning library, if it exports symbols explicitly"
    )
    parser.add_argument("--rust-output", help="Generated Rust module")
    args = parser.parse_args()
    if not args.output and not args.rust_output:
        parser.error("one of --output and --rust-output is required")
    if bool(args.output) != bool(args.implementation):
        parser.error("--output and --implementation must be used together")

    with open(args.input, "r", encoding="utf-8") as input_file:
        document = parse(input_file.read())

    if args.output:
        with (
            open(args.output, "w", encoding="utf-8") as output_file,
            open(args.implementation, "w", encoding="utf-8") as implementation_file,
        ):
            build(
                output_file,
                document,
                implementation_file,
                args.header_name or Path(args.output).name,
                args.library,
                args.export_macro,
            )

    if args.rust_output:
        with open(args.rust_output, "w", encoding="utf-8") as output_file:
            build_rust(output_file, document)


if __name__ == "__main__":
    main()
