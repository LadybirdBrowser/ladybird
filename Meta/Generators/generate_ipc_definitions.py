#!/usr/bin/env python3

# Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause


import argparse
import sys

from dataclasses import dataclass
from dataclasses import field
from pathlib import Path
from typing import List
from typing import Optional
from typing import TextIO

sys.path.append(str(Path(__file__).resolve().parent.parent))

from Utils.lexer import Lexer
from Utils.utils import string_hash

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
    "Web::HTML::AudioPlayState",
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
    includes: List[str] = field(default_factory=list)
    name: str = ""
    magic: int = 0
    messages: List[Message] = field(default_factory=list)
    # The argument a message can be routed by: messages whose first parameter is this one are about the
    # object it names, so a receiver can hand them to that object and a sender can bind a proxy to it.
    route: Optional[Parameter] = None

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


def parse(contents: str) -> List[Endpoint]:
    lexer = Lexer(contents)
    endpoints: List[Endpoint] = []

    def assert_specific(ch: str) -> None:
        if not lexer.consume_specific(ch):
            raise RuntimeError(f"Expected '{ch}' at position {lexer.position}, got '{lexer.peek()}'")

    def consume_whitespace() -> None:
        lexer.ignore_while(lambda c: c.isspace())
        if lexer.peek() == "/" and lexer.peek(1) == "/":
            lexer.ignore_until("\n")

    def parse_parameter_type() -> str:
        parameter_type = lexer.consume_until(lambda c: c == "<" or c.isspace())

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
            if lexer.consume_specific("]"):
                consume_whitespace()
                break

            if lexer.consume_specific(","):
                consume_whitespace()

            storage.append(lexer.consume_until(lambda c: c in ("]", ",")))
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

        endpoints[-1].includes.append(include)

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
        parse_includes()
        consume_whitespace()

        if not lexer.consume_specific("endpoint"):
            raise RuntimeError(f"Expected 'endpoint' at position {lexer.position}")

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

    while lexer.position < len(contents):
        parse_endpoint()

    for endpoint in endpoints:
        endpoint.validate()

    return endpoints


def constructor_for_message(name: str, parameters: List[Parameter]) -> str:
    if not parameters:
        return f"{name}() = default;"

    params = ", ".join(f"{p.type} {p.name}" for p in parameters)
    initializers = "\n        , ".join(f"m_{p.name}(move({p.name}))" for p in parameters)

    return f"""{name}({params})
        : {initializers}
    {{
    }}"""


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
    endpoint: Endpoint,
    name: str,
    parameters: List[Parameter],
    response_type: str = "",
) -> None:
    pascal_name = pascal_case(name)

    out.write(f"""
class {pascal_name} final : public IPC::Message {{
public:""")

    if response_type:
        out.write(f"\n    typedef class {response_type} ResponseType;\n")

    out.write(f"""
    {constructor_for_message(pascal_name, parameters)}

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

    out.write(f"""
    static constexpr u32 ENDPOINT_MAGIC = {endpoint.magic};

    virtual u32 endpoint_magic() const override {{ return ENDPOINT_MAGIC; }}
    virtual i32 message_id() const override {{ return (int)MessageID::{pascal_name}; }}
    static i32 static_message_id() {{ return (int)MessageID::{pascal_name}; }}
    virtual StringView message_name() const override {{ return "{endpoint.name}::{pascal_name}"sv; }}

    static ErrorOr<NonnullOwnPtr<{pascal_name}>> decode(Stream& stream, Queue<IPC::Attachment>& attachments)
    {{
        IPC::Decoder decoder {{ stream, attachments }};""")

    for parameter in parameters:
        out.write(f"\n        auto {parameter.name} = TRY((decoder.decode<{parameter.type}>()));")

        if "UTF8" in parameter.attributes:
            out.write(f"""
        if (!Utf8View({parameter.name}).validate())
            return Error::from_string_literal("Decoded {parameter.name} is invalid UTF-8");
""")

    constructor_args = ", ".join(f"move({p.name})" for p in parameters)
    out.write(f"""
        return make<{pascal_name}>({constructor_args});
    }}
""")

    encode_params = ", ".join(f"{make_argument_type(p.type_for_encoding)} {p.name}" for p in parameters)
    out.write(f"""
    static ErrorOr<IPC::MessageBuffer> static_encode({encode_params})
    {{
        IPC::MessageBuffer buffer;
        IPC::Encoder stream(buffer);
        TRY(stream.encode(ENDPOINT_MAGIC));
        TRY(stream.encode((int)MessageID::{pascal_name}));""")

    for parameter in parameters:
        out.write(f"\n        TRY(stream.encode({parameter.name}));")

    member_args = ", ".join(f"m_{p.name}" for p in parameters)
    out.write(f"""
        return buffer;
    }}

    virtual ErrorOr<IPC::MessageBuffer> encode() const override
    {{
        return static_encode({member_args});
    }}
""")

    for parameter in parameters:
        if is_primitive_or_simple_type(parameter.type):
            out.write(f"\n    {parameter.type} {parameter.name}() const {{ return m_{parameter.name}; }}\n")
        else:
            out.write(f"""
    {parameter.type} const& {parameter.name}() const {{ return m_{parameter.name}; }}
    {parameter.type} take_{parameter.name}() {{ return move(m_{parameter.name}); }}
""")

    out.write("\nprivate:")
    for parameter in parameters:
        out.write(f"\n    {parameter.type} m_{parameter.name};")
    out.write("\n};\n")


def write_proxy_method(
    out: TextIO,
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

    out.write(f"\n    {return_type} {method_name}({', '.join(signature_params)})\n    {{")

    if not is_synchronous and not is_try and not is_unicode_string_overload:
        for parameter in parameters:
            if parameter.type == "String" and parameter.type_for_encoding == "StringView":
                out.write(f"\n        VERIFY(Utf8View {{ {parameter.name} }}.validate());")
                generate_unicode_string_overload = True
            elif parameter.type == "Utf16String" and parameter.type_for_encoding == "Utf16View":
                out.write(f"\n        VERIFY({parameter.name}.validate());")
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
        sync_call = f"m_connection.template send_sync<Messages::{endpoint.name}::{pascal_name}>({call_args})"

        if return_type == "void":
            out.write(f"\n        (void){sync_call};")
        elif len(message.outputs) == 1:
            output = message.outputs[0]
            accessor = output.name if is_primitive_or_simple_type(output.type) else f"take_{output.name}"
            out.write(f"\n        return {sync_call}->{accessor}();")
        else:
            out.write(f"\n        return move(*{sync_call});")
    elif is_try:
        if inner_return_type == "void":
            success_return = "{}"
        elif len(message.outputs) == 1:
            output = message.outputs[0]
            accessor = output.name if is_primitive_or_simple_type(output.type) else f"take_{output.name}"
            success_return = f"result->{accessor}()"
        else:
            success_return = "move(*result)"
        out.write(f"""
        if (auto result = m_connection.template send_sync_but_allow_failure<Messages::{endpoint.name}::{pascal_name}>({call_args}))
            return {success_return};
        m_connection.shutdown();
        return IPC::ErrorCode::PeerDisconnected;""")
    else:
        # Async messages silently ignore send failures (e.g. peer disconnected).
        out.write(f"""
        auto message_buffer = MUST(Messages::{endpoint.name}::{pascal_name}::static_encode({call_args}));
        (void)m_connection.post_message(message_buffer);""")

    out.write("\n    }\n")

    if generate_unicode_string_overload:
        write_proxy_method(
            out,
            endpoint,
            message,
            message.inputs,
            is_synchronous,
            is_try,
            is_unicode_string_overload=True,
        )


def write_proxy_class(out: TextIO, endpoint: Endpoint) -> None:
    out.write(f"""
template<typename LocalEndpoint, typename PeerEndpoint>
class {endpoint.name}Proxy {{
public:
    // Used to disambiguate the constructor call.
    struct Tag {{ }};

    {endpoint.name}Proxy(IPC::Connection<LocalEndpoint, PeerEndpoint>& connection, Tag)
        : m_connection(connection)
    {{
    }}
""")

    for message in endpoint.messages:
        write_proxy_method(out, endpoint, message, message.inputs, message.is_synchronous, is_try=False)
        if message.is_synchronous:
            write_proxy_method(out, endpoint, message, message.inputs, is_synchronous=False, is_try=False)
            write_proxy_method(out, endpoint, message, message.inputs, is_synchronous=True, is_try=True)

    out.write("""
private:
    IPC::Connection<LocalEndpoint, PeerEndpoint>& m_connection;
};
""")


def write_endpoint_class(out: TextIO, endpoint: Endpoint) -> None:
    out.write(f"""
template<typename LocalEndpoint, typename PeerEndpoint>
class {endpoint.name}Proxy;
class {endpoint.name}Stub;

class {endpoint.name}Endpoint {{
public:
    template<typename LocalEndpoint>
    using Proxy = {endpoint.name}Proxy<LocalEndpoint, {endpoint.name}Endpoint>;
    using Stub = {endpoint.name}Stub;

    static u32 static_magic() {{ return {endpoint.magic}; }}

    static ErrorOr<NonnullOwnPtr<IPC::Message>> decode_message(ReadonlyBytes buffer, [[maybe_unused]] Queue<IPC::Attachment>& attachments)
    {{
        FixedMemoryStream stream {{ buffer }};
        auto message_endpoint_magic = TRY(stream.read_value<u32>());

        if (message_endpoint_magic != static_magic())
            return Error::from_string_literal("Endpoint magic number mismatch, not my message!");

        auto message_id = TRY(stream.read_value<i32>());

        switch (message_id) {{""")

    for message in endpoint.messages:
        names = [message.name]
        if message.is_synchronous:
            names.append(message.response_name())

        for name in names:
            pascal_name = pascal_case(name)
            out.write(f"""
        case (int)Messages::{endpoint.name}::MessageID::{pascal_name}:
            return Messages::{endpoint.name}::{pascal_name}::decode(stream, attachments);""")

    out.write(f"""
        default:
            return Error::from_string_literal("Failed to decode {endpoint.name} message");
        }}

        VERIFY_NOT_REACHED();
    }}
}};
""")


def write_stub_class(out: TextIO, endpoint: Endpoint) -> None:
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
class {endpoint.name}Stub : {bases} {{
public:
    {endpoint.name}Stub() {{ }}
    virtual ~{endpoint.name}Stub() override {{ }}

    virtual u32 magic() const override {{ return {endpoint.magic}; }}
    virtual ByteString name() const override {{ return "{endpoint.name}"; }}

    virtual ErrorOr<OwnPtr<IPC::MessageBuffer>> handle(NonnullOwnPtr<IPC::Message> message) override
    {{
        switch (message->message_id()) {{""")

    for message in endpoint.messages:
        pascal_name = pascal_case(message.name)
        out.write(f"\n        case (int)Messages::{endpoint.name}::MessageID::{pascal_name}:\n")

        if message.inputs:
            out.write(f"            return handle_{message.name}(*message);")
        else:
            out.write(f"            return handle_{message.name}();")

    out.write(f"""
        default:
            return Error::from_string_literal("Unknown message ID for {endpoint.name} endpoint");
        }}
    }}
""")

    for message in endpoint.messages:
        pascal_name = pascal_case(message.name)

        arg_parts: List[str] = []
        for parameter in message.inputs:
            accessor = parameter.name if is_primitive_or_simple_type(parameter.type) else f"take_{parameter.name}"
            arg_parts.append(f"request.{accessor}()")
        arguments = ", ".join(arg_parts)

        out.write(f"\n    NEVER_INLINE ErrorOr<OwnPtr<IPC::MessageBuffer>> handle_{message.name}(")

        if message.inputs:
            out.write("IPC::Message& message)\n    {\n")
            out.write(f"        auto& request = static_cast<Messages::{endpoint.name}::{pascal_name}&>(message);\n")
        else:
            out.write(")\n    {\n")

        # An argument whose type is a claim by the sender about itself is verified before the handler
        # runs. Which types make a claim is decided by the types, not here.
        for parameter in message.inputs:
            reason = f"argument '{parameter.name}' is not a valid claim for this sender"
            out.write(f"        if (!IPC::verify_message_argument(*this, request.{parameter.name}())) {{\n")
            out.write(f'            did_misbehave("{message.name}"sv, "{reason}"sv);\n')

            if not message.is_synchronous:
                out.write("            return nullptr;\n")
            else:
                refused_types = [message_name_qualified(endpoint.name, message.name, True)]
                refused_types += [output.type for output in message.outputs]
                out.write(f"            return IPC::refused_reply<{', '.join(refused_types)}>();\n")

            out.write("        }\n")

        if not message.is_synchronous:
            out.write(f"        {message.name}({arguments});\n")
            out.write("        return nullptr;\n")
        elif not message.outputs:
            response_pascal_name = pascal_case(message.response_name())
            out.write(f"        {message.name}({arguments});\n")
            out.write(f"        auto response = Messages::{endpoint.name}::{response_pascal_name} {{}};\n")
            out.write("        return make<IPC::MessageBuffer>(TRY(response.encode()));\n")
        else:
            out.write(f"        auto response = {message.name}({arguments});\n")
            out.write("        return make<IPC::MessageBuffer>(TRY(response.encode()));\n")

        out.write("    }\n")

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
class {endpoint.name}{scope}Stub {{
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


def write_routing_stub_mixin(out: TextIO, endpoint: Endpoint) -> None:
    assert endpoint.route is not None
    scope = endpoint.route_scope_pascal()
    route = endpoint.route
    stub_accessor = f"{endpoint.route_scope()}_stub"

    out.write(f"""
// A receiver that hands each message about a {endpoint.route_scope()} to that {endpoint.route_scope()}'s stub, and drops
// it when there is none. It still answers the unrouted messages itself, and may take a routed one back by
// overriding it.
template<typename Base>
class {endpoint.name}{scope}RoutingStub : public Base {{
public:
    using Base::Base;

    virtual {endpoint.name}{scope}Stub* {stub_accessor}({route.type} const&) = 0;
""")

    for message in endpoint.messages:
        if not endpoint.routes(message):
            continue
        params = ", ".join(f"{p.type} {p.name}" for p in message.inputs)
        args = ", ".join(
            p.name if is_primitive_or_simple_type(p.type) else f"move({p.name})" for p in message.inputs[1:]
        )
        out.write(f"\n    virtual void {message.name}({params}) override\n    {{\n")
        out.write(
            f"        if (auto* stub = {stub_accessor}({route.name}))\n            stub->{message.name}({args});\n"
        )
        out.write("    }\n")

    out.write("};\n")


def write_bound_proxy_method(
    out: TextIO,
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
    call_args: List[str] = [f"derived().routed_{endpoint.route.name}()"]
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
    out.write(f"\n    {return_type} {method_name}({', '.join(signature_params)}) const\n    {{\n")
    out.write("        if (auto* connection = derived().routed_connection())\n")
    if return_type == "void":
        out.write(f"            {call};\n")
    else:
        out.write(f"            return {call};\n")
        if is_try:
            out.write("        return IPC::ErrorCode::PeerDisconnected;\n")
        elif len(message.outputs) == 1:
            out.write(f"        return IPC::empty_value<{return_type}>();\n")
        else:
            outputs = ", ".join(output.type for output in message.outputs)
            out.write(f"        return IPC::empty_response<{return_type}, {outputs}>();\n")
    out.write("    }\n")

    if generate_unicode_string_overload:
        write_bound_proxy_method(out, endpoint, message, is_synchronous, is_try, is_unicode_string_overload=True)


def write_bound_proxy_class(out: TextIO, endpoint: Endpoint) -> None:
    assert endpoint.route is not None
    scope = endpoint.route_scope_pascal()

    out.write(f"""
// Sends the messages about one {endpoint.route_scope()} without naming it each time. The class deriving this provides
// routed_connection(), the proxy the messages go through, or null once the {endpoint.route_scope()} has none, and
// routed_{endpoint.route.name}(). A message for a {endpoint.route_scope()} without a connection is dropped.
template<typename Derived>
class {endpoint.name}{scope}Proxy {{
public:
""")

    for message in endpoint.messages:
        if not endpoint.binds(message):
            continue
        write_bound_proxy_method(out, endpoint, message, message.is_synchronous, is_try=False)
        if message.is_synchronous:
            write_bound_proxy_method(out, endpoint, message, is_synchronous=False, is_try=False)
            write_bound_proxy_method(out, endpoint, message, is_synchronous=True, is_try=True)

    out.write("""
private:
    Derived const& derived() const { return static_cast<Derived const&>(*this); }
};
""")


def write_endpoint(out: TextIO, endpoint: Endpoint) -> None:
    out.write(f"\nnamespace Messages::{endpoint.name} {{\n")
    write_message_ids_enum(out, endpoint)

    for message in endpoint.messages:
        response_name = ""

        if message.is_synchronous:
            response_name = message.response_name()
            write_message_class(out, endpoint, response_name, message.outputs)

        write_message_class(out, endpoint, message.name, message.inputs, response_name)

    out.write(f"\n}} // namespace Messages::{endpoint.name}\n")

    write_proxy_class(out, endpoint)
    write_endpoint_class(out, endpoint)
    write_stub_class(out, endpoint)
    if endpoint.route is not None:
        write_routed_stub_class(out, endpoint)
        write_routing_stub_mixin(out, endpoint)
        write_bound_proxy_class(out, endpoint)

    out.write("""
#if defined(AK_COMPILER_CLANG)
#pragma clang diagnostic pop
#endif
""")


def build(out: TextIO, endpoints: List[Endpoint]) -> None:
    out.write("#pragma once\n\n")

    for endpoint in endpoints:
        for include in endpoint.includes:
            out.write(f"{include}\n")

    out.write("""\
#include <AK/Error.h>
#include <AK/MemoryStream.h>
#include <AK/OwnPtr.h>
#include <AK/Platform.h>
#include <AK/Result.h>
#include <AK/Utf8View.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Connection.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/File.h>
#include <LibIPC/Message.h>
#include <LibIPC/SenderClaim.h>
#include <LibIPC/Stub.h>

#if defined(AK_COMPILER_CLANG)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdefaulted-function-deleted"
#endif
""")

    for endpoint in endpoints:
        write_endpoint(out, endpoint)


def main():
    parser = argparse.ArgumentParser(description="Ladybird IPC endpoint compiler")
    parser.add_argument("--input", required=True, help="IPC endpoint definition file")
    parser.add_argument("--output", required=True, help="File to write file generated header")
    args = parser.parse_args()

    with open(args.input, "r", encoding="utf-8") as input_file:
        endpoints = parse(input_file.read())

    with open(args.output, "w", encoding="utf-8") as output_file:
        build(output_file, endpoints)


if __name__ == "__main__":
    main()
