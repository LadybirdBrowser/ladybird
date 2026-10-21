import io
import os
import sys
import unittest

from pathlib import Path

sys.path.insert(0, str(Path(os.environ["LADYBIRD_SOURCE_DIR"]) / "Meta"))

from Generators.generate_ipc_definitions import build
from Generators.generate_ipc_definitions import build_rust
from Generators.generate_ipc_definitions import parse
from Generators.generate_ipc_definitions import rust_type

ENDPOINT = """\
endpoint TestClient {
    page_changed(Web::PageId page_id, u32 revision) =|
    page_wants_cookie(Web::PageId page_id) => (HTTP::Cookie::VersionedCookie cookie)
    ping() =|
}
"""


def generate_files(contents: str) -> tuple[str, str]:
    out = io.StringIO()
    implementation = io.StringIO()
    build(out, parse(contents), implementation, "TestEndpoint.h")
    return out.getvalue(), implementation.getvalue()


def generate(contents: str) -> str:
    header, implementation = generate_files(contents)
    return header + implementation


class TestIPCGenerator(unittest.TestCase):
    def test_non_template_implementations_are_only_in_the_cpp_file(self) -> None:
        header, implementation = generate_files(ENDPOINT)
        self.assertIn('#include "TestEndpoint.h"', implementation)
        self.assertIn("static ErrorOr<NonnullOwnPtr<PageChanged>> decode(AK::FixedMemoryStream& stream", header)
        self.assertNotIn("decoder.decode<", header)
        self.assertNotIn("stream.encode(", header)
        self.assertNotIn("IPC::verify_message_argument", header)
        self.assertNotIn("m_connection.send_sync", header)
        self.assertIn("PageChanged::decode(AK::FixedMemoryStream& stream", implementation)
        self.assertIn("TestClientProxy::page_wants_cookie", implementation)
        self.assertIn("TestClientStub::handle_page_changed", implementation)
        self.assertNotIn("template<typename LocalEndpoint", header)

    def test_exports_are_applied_to_the_generated_api(self) -> None:
        header = io.StringIO()
        implementation = io.StringIO()
        build(header, parse(ROUTED_ENDPOINT), implementation, "TestEndpoint.h", "LibWebCommon")
        self.assertIn(
            """\
#if __has_include(<LibWebCommon/Export.h>)
#include <LibWebCommon/Export.h>
#else
#define WEBCOMMON_API
#endif
""",
            header.getvalue(),
        )
        for name in [
            "PageChanged",
            "TestClientProxy",
            "TestClientEndpoint",
            "TestClientStub",
            "TestClientPageRoutingStub",
            "TestClientPageProxy",
        ]:
            self.assertIn(f"class WEBCOMMON_API {name}", header.getvalue())
        self.assertNotIn("class WEBCOMMON_API TestClientProxy;", header.getvalue())
        self.assertNotIn("template<typename Derived>", header.getvalue())
        self.assertNotIn("template<typename Base>", header.getvalue())

    def test_asynchronous_message_verifies_every_argument_before_dispatch(self) -> None:
        generated = generate(ENDPOINT)

        for argument in ("page_id", "revision"):
            self.assertIn(
                f"""\
    if (!IPC::verify_message_argument(*this, request.{argument}())) {{
        did_misbehave("page_changed"sv, "argument '{argument}' is not a valid claim for this sender"sv);
        return nullptr;
    }}
""",
                generated,
            )

        verification = generated.index("if (!IPC::verify_message_argument(*this, request.revision()))")
        dispatch = generated.index("page_changed(request.take_page_id(), request.revision());")
        self.assertLess(verification, dispatch)

    def test_synchronous_message_refuses_with_an_empty_reply(self) -> None:
        generated = generate(ENDPOINT)

        self.assertIn(
            """\
    if (!IPC::verify_message_argument(*this, request.page_id())) {
        did_misbehave("page_wants_cookie"sv, "argument 'page_id' is not a valid claim for this sender"sv);
        return IPC::refused_reply<Messages::TestClient::PageWantsCookieResponse, HTTP::Cookie::VersionedCookie>();
    }
""",
            generated,
        )

    def test_message_without_arguments_is_not_verified(self) -> None:
        generated = generate(ENDPOINT)

        self.assertIn(
            """\
ErrorOr<OwnPtr<IPC::MessageBuffer>> TestClientStub::handle_ping()
{
    ping();
    return nullptr;
}
""",
            generated,
        )


ROUTED_ENDPOINT = """\
endpoint TestClient routed by Web::PageId page_id {
    page_changed(Web::PageId page_id, u32 revision) =|
    page_acknowledged(Web::PageId page_id) => ()
    [NotRouted] page_wants_cookie(Web::PageId page_id) => (HTTP::Cookie::VersionedCookie cookie)
    ping() =|
}
"""

ROUTED_SYNCHRONOUS_ENDPOINT = """\
endpoint TestClient routed by Web::PageId page_id {
    page_wants_cookie(Web::PageId page_id) => (HTTP::Cookie::VersionedCookie cookie)
}
"""


class TestIPCGeneratorRouting(unittest.TestCase):
    def test_routed_messages_get_a_stub_for_the_object_they_are_about(self) -> None:
        generated = generate(ROUTED_ENDPOINT)
        self.assertIn("class TestClientPageStub {", generated)
        self.assertIn("    virtual void page_changed(u32 revision) = 0;\n", generated)
        self.assertIn("    virtual void page_acknowledged() = 0;\n", generated)
        self.assertNotIn("    virtual void ping() = 0;\n};\n\n// A receiver that hands", generated)

    def test_routing_stub_hands_a_message_to_the_page_or_drops_it(self) -> None:
        generated = generate(ROUTED_ENDPOINT)
        self.assertIn("virtual TestClientPageStub* page_stub(Web::PageId const&) = 0;", generated)
        self.assertIn(
            """\
void TestClientPageRoutingStub::page_changed(Web::PageId page_id, u32 revision)
{
    if (auto* stub = page_stub(page_id))
        stub->page_changed(revision);
}
""",
            generated,
        )
        self.assertIn(
            """\
void TestClientPageRoutingStub::page_acknowledged(Web::PageId page_id)
{
    if (auto* stub = page_stub(page_id))
        stub->page_acknowledged();
}
""",
            generated,
        )
        self.assertNotIn("IPC::empty_response", generated)

    def test_bound_proxy_sends_without_naming_the_page(self) -> None:
        generated = generate(ROUTED_ENDPOINT)
        self.assertIn("class TestClientPageProxy {", generated)
        self.assertIn(
            """\
void TestClientPageProxy::async_page_changed(u32 revision) const
{
    if (auto* connection = routed_proxy())
        connection->async_page_changed(routed_page_id(), revision);
}
""",
            generated,
        )
        self.assertIn("    HTTP::Cookie::VersionedCookie page_wants_cookie() const;\n", generated)
        header, _ = generate_files(ROUTED_ENDPOINT)
        self.assertNotIn("async_ping", header.split("class TestClientPageProxy {")[1])

    def test_a_message_marked_not_routed_stays_on_the_connection(self) -> None:
        generated = generate(ROUTED_ENDPOINT)
        stub = generated.split("class TestClientPageStub {")[1].split("};")[0]
        self.assertNotIn("page_wants_cookie", stub)
        self.assertIn(
            "    virtual Messages::TestClient::PageWantsCookieResponse page_wants_cookie(Web::PageId page_id) = 0;\n",
            generated,
        )

    def test_a_message_the_connection_answers_is_still_sent_about_a_page(self) -> None:
        generated = generate(ROUTED_ENDPOINT)
        proxy = generated.split("class TestClientPageProxy {")[1]
        self.assertIn("page_wants_cookie", proxy)

    def test_a_synchronous_message_returning_values_cannot_be_routed(self) -> None:
        with self.assertRaises(RuntimeError) as raised:
            generate(ROUTED_SYNCHRONOUS_ENDPOINT)
        self.assertIn("cannot be routed", str(raised.exception))


DECLARATIONS = """\
#include <LibIPC/File.h>

enum Test::Shape : u8 { Circle, Square }

struct Test::Record {
    [NonZero] u64 id;
    [MaxBytes=8] String label;
    [MaxCount=3] Vector<u16> values;
    [Finite] double scale;
    Test::Shape shape;
}

variant Test::Value { Record Test::Record, Number u32, File IPC::File }

[ExistingCpp]
struct Web::Existing {
    Optional<Test::Shape> shape;
}

[ExistingCpp, Accessors]
struct Gfx::IntSize {
    i32 width;
    i32 height;
}
"""


def generate_rust(contents: str) -> str:
    output = io.StringIO()
    build_rust(output, parse(contents))
    return output.getvalue()


class TestIPCGeneratorDeclarations(unittest.TestCase):
    def test_declared_types_are_defined_in_cpp(self) -> None:
        header, _ = generate_files(DECLARATIONS)
        self.assertIn(
            """\
namespace Test {

enum class Shape : u8 {
    Circle,
    Square,
};

struct Record {
    u64 id {};
    String label {};
    Vector<u16> values {};
    double scale {};
    Test::Shape shape {};
};

using Value = Variant<Test::Record, u32, IPC::File>;

} // namespace Test
""",
            header,
        )
        self.assertNotIn("struct Existing", header)
        self.assertNotIn("class IntSize", header)
        self.assertNotIn("LibIPC/Stub.h", header)
        self.assertIn("#include <LibIPC/Forward.h>", header)

    def test_codecs_are_declared_for_structs_and_enums_but_not_variants(self) -> None:
        header = io.StringIO()
        build(header, parse(DECLARATIONS), io.StringIO(), "TestTypes.h", "LibWebCommon", "WEBCOMMON_API")
        self.assertIn("#pragma once\n\n#include <LibWebCommon/Export.h>\n", header.getvalue())
        self.assertNotIn("#define", header.getvalue())
        for name in ["Test::Shape", "Test::Record", "Web::Existing", "Gfx::IntSize"]:
            self.assertIn(
                f"template<>\nWEBCOMMON_API ErrorOr<void> encode(Encoder&, {name} const&);", header.getvalue()
            )
            self.assertIn(f"template<>\nWEBCOMMON_API ErrorOr<{name}> decode(Decoder&);", header.getvalue())
        self.assertNotIn("Test::Value const&", header.getvalue())

    def test_codecs_of_libraries_without_explicit_exports_are_not_exported(self) -> None:
        header = io.StringIO()
        build(header, parse(DECLARATIONS), io.StringIO(), "TestTypes.h", "LibGfx")
        self.assertNotIn("Export.h", header.getvalue())
        self.assertNotIn("GFX_API", header.getvalue())
        self.assertIn("template<>\nErrorOr<void> encode(Encoder&, Gfx::IntSize const&);", header.getvalue())

    def test_enums_go_as_the_position_of_their_value(self) -> None:
        _, implementation = generate_files(DECLARATIONS)
        self.assertIn(
            """\
    switch (value) {
    case Test::Shape::Circle:
        return encoder.encode<u8>(0);
    case Test::Shape::Square:
        return encoder.encode<u8>(1);
    }
""",
            implementation,
        )
        self.assertIn(
            """\
    switch (TRY(decoder.decode<u8>())) {
    case 0:
        return Test::Shape::Circle;
    case 1:
        return Test::Shape::Square;
    default:
        return Error::from_string_literal("IPC decode: Invalid Test::Shape");
    }
""",
            implementation,
        )
        rust = generate_rust(DECLARATIONS)
        self.assertIn("#[repr(u8)]\npub enum Test_Shape {\n    Circle,\n    Square,\n}", rust)
        self.assertIn("encoder.encode(&(*self as u8))", rust)
        self.assertIn("1 => ::std::result::Result::Ok(Self::Square),", rust)
        self.assertIn(
            '_ => ::std::result::Result::Err(::libipc_rust::wire::DecodeError::InvalidValue("Test::Shape")),', rust
        )

    def test_sizes_are_bounded_before_contents_and_other_fields_are_checked_after_decoding(self) -> None:
        _, implementation = generate_files(DECLARATIONS)
        self.assertIn(
            """\
    auto value = Test::Record {
        .id = TRY((decoder.decode<u64>())),
        .label = TRY((decoder.decode_with_max_size<String>(8))),
        .values = TRY((decoder.decode_with_max_size<Vector<u16>>(3))),
        .scale = TRY((decoder.decode<double>())),
        .shape = TRY((decoder.decode<Test::Shape>())),
    };
    if (value.id == 0)
        return Error::from_string_literal("IPC decode: Test::Record::id is zero");
    if (!isfinite(value.scale))
        return Error::from_string_literal("IPC decode: Test::Record::scale is not finite");
    return value;
""",
            implementation,
        )
        rust = generate_rust(DECLARATIONS)
        self.assertIn(
            """\
        let value = Self {
            id: decoder.decode()?,
            label: decoder.decode_with_max_size(8)?,
            values: decoder.decode_with_max_size(3)?,
            scale: decoder.decode()?,
            shape: decoder.decode()?,
        };
""",
            rust,
        )
        for condition, field in [("value.id == 0", "id"), ("!value.scale.is_finite()", "scale")]:
            self.assertIn(
                f'        if {condition} {{\n            return ::std::result::Result::Err(::libipc_rust::wire::DecodeError::InvalidValue("Test::Record::{field}"));',
                rust,
            )

    def test_encoders_refuse_what_decoders_refuse(self) -> None:
        _, implementation = generate_files(DECLARATIONS)
        for condition, message in [
            ("value.id == 0", "id is zero"),
            ("value.label.bytes().size() > 8", "label is too large"),
            ("value.values.size() > 3", "values is too large"),
            ("!isfinite(value.scale)", "scale is not finite"),
        ]:
            self.assertIn(
                f'    if ({condition})\n        return Error::from_string_literal("IPC encode: Test::Record::{message}");\n',
                implementation,
            )
        rust = generate_rust(DECLARATIONS)
        for condition, field in [
            ("self.id == 0", "id"),
            ("self.label.as_bytes().len() > 8", "label"),
            ("self.values.len() > 3", "values"),
            ("!self.scale.is_finite()", "scale"),
        ]:
            self.assertIn(
                f'        if {condition} {{\n            return ::std::result::Result::Err(::libipc_rust::wire::EncodeError::InvalidValue("Test::Record::{field}"));',
                rust,
            )
        self.assertEqual(rust.count("        self.validate()?;\n"), 2)
        for cpp_type, cpp_size, rust_size in [
            ("ByteString", "length()", "0.len()"),
            ("ByteBuffer", "size()", "len()"),
        ]:
            _, implementation = generate_files(f"struct Test::Text {{ [MaxBytes=4] {cpp_type} text; }}")
            self.assertIn(f"if (value.text.{cpp_size} > 4)", implementation)
            self.assertIn(
                f"if self.text.{rust_size} > 4 {{",
                generate_rust(f"struct Test::Text {{ [MaxBytes=4] {cpp_type} text; }}"),
            )

    def test_definitions_include_the_ak_types_their_fields_hold(self) -> None:
        header, _ = generate_files("struct Test::Map { HashMap<u32, Optional<String>> entries; IPC::File file; }")
        for name in ["HashMap", "Optional", "String"]:
            self.assertIn(f"#include <AK/{name}.h>\n", header)
        self.assertNotIn("#include <AK/File.h>", header)

    def test_declared_codecs_use_the_export_macro_beside_endpoints(self) -> None:
        header = io.StringIO()
        contents = "struct Test::Value { u32 number; } endpoint Test { send(Test::Value value) =| }"
        build(header, parse(contents), io.StringIO(), "TestEndpoint.h", "LibWebCommon", "WEBCOMMON_API")
        self.assertIn(
            "template<>\nWEBCOMMON_API ErrorOr<void> encode(Encoder&, Test::Value const&);", header.getvalue()
        )
        header = io.StringIO()
        build(header, parse(contents), io.StringIO(), "TestEndpoint.h", "LibGfx")
        self.assertIn("template<>\nErrorOr<void> encode(Encoder&, Test::Value const&);", header.getvalue())
        self.assertIn("class GFX_API Test", header.getvalue())

    def test_encoders_check_that_fields_have_their_declared_types(self) -> None:
        _, implementation = generate_files(DECLARATIONS)
        self.assertIn("    static_assert(IsSame<decltype(value.label), String>);\n", implementation)
        self.assertIn("    static_assert(IsSame<RemoveCVReference<decltype(value.width())>, i32>);\n", implementation)

    def test_existing_cpp_types_are_read_as_they_are_built(self) -> None:
        _, implementation = generate_files(DECLARATIONS)
        self.assertIn("TRY(encoder.encode(value.shape));", implementation)
        self.assertIn(
            "return Web::Existing {\n        .shape = TRY((decoder.decode<Optional<Test::Shape>>())),\n    };",
            implementation,
        )
        self.assertIn("TRY(encoder.encode(value.width()));", implementation)
        self.assertIn(
            "return Gfx::IntSize {\n        TRY((decoder.decode<i32>())),\n        TRY((decoder.decode<i32>())),\n    };",
            implementation,
        )
        _, implementation = generate_files("struct Test::Map { HashMap<u32, u32> entries; }")
        self.assertIn(".entries = TRY((decoder.decode<HashMap<u32, u32>>())),", implementation)

    def test_variants_go_as_the_index_of_their_alternative(self) -> None:
        rust = generate_rust(DECLARATIONS)
        self.assertIn(
            "pub enum Test_Value {\n    Record(crate::ipc_types::Test_Record),\n    Number(u32),\n    File(::libipc_rust::wire::File),\n}",
            rust,
        )
        self.assertIn("            Self::File(value) => {\n                encoder.encode(&2u8)?;\n", rust)
        self.assertIn("            2 => ::std::result::Result::Ok(Self::File(decoder.decode()?)),\n", rust)
        self.assertIn('InvalidValue("Test::Value")', rust)

    def test_endpoints_use_declared_types_through_the_crate(self) -> None:
        contents = DECLARATIONS + "endpoint Test { echo(Test::Value value) => (Test::Value value) }"
        header, _ = generate_files(contents)
        self.assertLess(header.index("using Value ="), header.index("namespace Messages::Test"))
        self.assertIn("pub value: crate::ipc_types::Test_Value,", generate_rust(contents))

    def test_names_and_attributes_are_read_without_surrounding_spaces(self) -> None:
        document = parse("[ ExistingCpp , Accessors ] struct Gfx::IntSize { [ NonZero ] i32 width; }")
        self.assertEqual(document.declarations[0].attributes, ["ExistingCpp", "Accessors"])
        self.assertEqual(document.declarations[0].members[0].attributes, ["NonZero"])
        document = parse("enum Test::Kind: u8 { A }")
        self.assertEqual(document.declarations[0].name, "Test::Kind")
        self.assertEqual(document.declarations[0].underlying_type, "u8")

    def test_invalid_declarations_are_rejected(self) -> None:
        for contents, error in [
            ("struct Record { u32 id; }", "with a namespace"),
            ("enum Test::Kind { A }", "Expected ':'"),
            ("enum Test::Kind : int { A }", "sized integer"),
            ("enum Test::Kind : u8 { }", "Empty enum"),
            ("enum Test::Kind : i8 { " + ", ".join(f"V{i}" for i in range(129)) + " }", "more values than i8"),
            ("variant Test::Value { A u32, B u32 }", "distinct type"),
            ("variant Test::Value { " + ", ".join(f"V{i} Test::T{i}" for i in range(255)) + " }", "too many"),
            ("struct Test::Record { u32 id; u32 id; }", "Duplicate member"),
            ("struct Test::Record { u32 id }", "Expected ';'"),
            ("struct Test::A { } struct Test::A { }", "Duplicate type"),
            ("[Accessors] struct Test::Record { u32 id; }", "can have accessors"),
            ("[ExistingCpp, Accessors] enum Test::Kind : u8 { A }", "can have accessors"),
            ("[Unknown] struct Test::Record { u32 id; }", "Unknown attribute"),
            ("[ExistingCpp] endpoint Test { ping() =| }", "Expected 'endpoint', 'struct'"),
            ("struct Test::Record { [Bounded] u32 id; }", "Unknown attribute"),
            ("struct Test::Record { [MaxBytes=8] Vector<u8> bytes; }", "MaxBytes cannot apply"),
            ("struct Test::Record { [MaxBytes=0] String text; }", "needs a size"),
            ("struct Test::Record { [MaxCount=4294967296] Vector<u32> values; }", "needs a size"),
            ("struct Test::Record { [MaxCount] Vector<u32> values; }", "needs a size"),
            ("struct Test::Record { [NonZero] double value; }", "NonZero cannot apply"),
            ("struct Test::Record { [NonZero=1] u32 value; }", "takes no argument"),
            ("struct Test::Record { [Finite] u32 value; }", "Finite cannot apply"),
            ("struct Test::Record { unsigned int value; }", "Expected ';'"),
            ("[ExistingCpp", "Unexpected EOF in attributes"),
            ("variant Test::Value { A int, B i32 }", "distinct type"),
            ("variant Test::Value { A unsigned, B u32 }", "distinct type"),
            ("variant Test::Value { A Test::R, B ::Test::R }", "distinct type"),
            ("variant Test::Value { A, B u32 }", "Invalid type '' of Test::Value::A"),
            ("struct Test::Record { ; }", "Expected ';'"),
            ("struct Test::Record { Vector<> values; }", "Invalid type 'Vector<>' of Test::Record::values"),
            ("struct Test::Record { 1u32 value; }", "Invalid type '1u32' of Test::Record::value"),
            ("struct Test::A_B { } struct Test::A::B { }", "same Rust name"),
            ("struct Test::Record { [NonZero u32 value; }", "Unexpected EOF in attributes"),
        ]:
            with self.subTest(contents=contents):
                with self.assertRaisesRegex(RuntimeError, error):
                    parse(contents)


class TestIPCRustGenerator(unittest.TestCase):
    def generate(self, contents: str) -> str:
        output = io.StringIO()
        build_rust(output, parse(contents))
        return output.getvalue()

    def test_message_numbers_match_cpp_including_synchronous_replies(self) -> None:
        generated = self.generate(ENDPOINT)
        for name, number in [("PageChanged", 1), ("PageWantsCookie", 2), ("PageWantsCookieResponse", 3), ("Ping", 4)]:
            self.assertIn(
                f"impl ::libipc_rust::wire::EndpointMessage for {name} {{\n            const ENDPOINT_MAGIC: u32 = super::ENDPOINT_MAGIC;\n            const MESSAGE_ID: i32 = {number};",
                generated,
            )
        self.assertIn(f"pub const ENDPOINT_MAGIC: u32 = {parse(ENDPOINT).endpoints[0].magic};", generated)

    def test_types_compose_and_custom_types_are_supplied_by_the_crate(self) -> None:
        self.assertEqual(rust_type("Vector<Optional<i64>>"), "::std::vec::Vec<::std::option::Option<i64>>")
        self.assertEqual(rust_type("Array<u8, 16>"), "[u8; 16]")
        self.assertEqual(rust_type("Web::PageId"), "crate::ipc_types::Web_PageId")
        generated = self.generate("endpoint Test { update(Vector<Optional<i64>> values, u32 type) =| }")
        self.assertIn("pub values: ::std::vec::Vec<::std::option::Option<i64>>", generated)
        self.assertIn("pub r#type: u32", generated)
        self.assertIn("_encoder.encode(&self.r#type)?;", generated)

    def test_unmapped_types_are_rejected(self) -> None:
        for cpp_type in [
            "u128",
            "char",
            "long",
            "CustomType",
            "Vector<u128>",
            "Optional<Unknown>",
            "Web::PageId*",
            "Foo::",
            "Foo::::Bar",
        ]:
            with self.subTest(cpp_type=cpp_type):
                with self.assertRaisesRegex(RuntimeError, "No Rust type for"):
                    rust_type(cpp_type)
        self.assertEqual(rust_type("::Web::PageId"), "crate::ipc_types::Web_PageId")

    def test_consumer_types_do_not_require_debug(self) -> None:
        generated = self.generate("endpoint Test { send(Test::Value value) => (Test::Value value) }")
        self.assertNotIn("derive(Debug)", generated)

    def test_empty_messages_and_multiple_endpoints_get_separate_modules(self) -> None:
        generated = self.generate("endpoint First { ping() => () } endpoint Second { stop() =| }")
        self.assertIn("pub mod first", generated)
        self.assertIn("pub mod second", generated)
        for name in ["Ping", "PingResponse", "Stop"]:
            self.assertIn(f"pub struct {name};", generated)

    def test_keywords_use_raw_identifiers(self) -> None:
        for keyword in (
            "abstract",
            "become",
            "box",
            "do",
            "final",
            "gen",
            "macro",
            "override",
            "priv",
            "try",
            "typeof",
            "unsized",
            "virtual",
            "yield",
        ):
            with self.subTest(keyword=keyword):
                generated = self.generate(f"endpoint {keyword} {{ update(u32 {keyword}) =| }}")
                self.assertIn(f"pub mod r#{keyword}", generated)
                self.assertIn(f"pub r#{keyword}: u32", generated)

    def test_path_keywords_are_rejected(self) -> None:
        for keyword in ("self", "super", "crate", "Self", "_"):
            with self.subTest(keyword=keyword):
                with self.assertRaisesRegex(RuntimeError, "Cannot use.*as a Rust identifier"):
                    self.generate(f"endpoint Test {{ update(u32 {keyword}) =| }}")
        with self.assertRaisesRegex(RuntimeError, "Cannot use 'Self'"):
            self.generate("endpoint Test { self() =| }")

    def test_message_types_do_not_shadow_generator_names(self) -> None:
        names = (
            "message",
            "attachment",
            "encode",
            "decode",
            "encoder",
            "decoder",
            "endpoint_message",
            "encode_error",
            "decode_error",
            "vec",
            "option",
            "result",
            "string",
        )
        generated = self.generate("endpoint Test { " + " ".join(f"{name}(String value) =|" for name in names) + " }")
        self.assertIn("pub mod messages {", generated)
        self.assertIn("Message(messages::Message)", generated)
        self.assertIn("impl ::libipc_rust::wire::Encode for Encode", generated)
        self.assertIn("pub value: ::libipc_rust::wire::Utf8String", generated)
        self.assertNotIn("use libipc_rust", generated)

    def test_decoder_checks_endpoint_and_message_number_but_allows_trailing_bytes(self) -> None:
        generated = self.generate(ENDPOINT)
        self.assertIn("if magic != ENDPOINT_MAGIC", generated)
        self.assertIn('InvalidValue("message ID")', generated)
        self.assertNotIn("decoder.is_at_end()", generated)

    def test_owned_encoding_moves_every_field(self) -> None:
        generated = self.generate("endpoint Test { send(Vector<IPC::File> files) =| }")
        self.assertIn("fn encode_owned(self,", generated)
        self.assertIn("_encoder.encode_owned(self.files)?;", generated)

    def test_utf8_is_validated_on_requests_and_responses(self) -> None:
        for cpp_type, expression in [
            ("ByteString", "&value.0"),
            ("ByteBuffer", "&value"),
        ]:
            with self.subTest(cpp_type=cpp_type):
                generated = self.generate(
                    f"endpoint Test {{ echo([UTF8] {cpp_type} text) => ([UTF8] {cpp_type} text) }}"
                )
                self.assertEqual(generated.count(f"::libipc_rust::wire::validate_utf8({expression})?;"), 2)
                encode_expression = expression.replace("value", "self.text")
                self.assertEqual(
                    generated.count(f"::libipc_rust::wire::validate_utf8({encode_expression}).map_err("), 2
                )
                self.assertEqual(generated.count('EncodeError::InvalidValue("UTF-8")'), 2)
                self.assertEqual(generated.count("fn validate_utf8(&self)"), 2)
                self.assertEqual(generated.count("self.validate_utf8()?;"), 4)
                self.assertEqual(
                    generate(f"endpoint Test {{ echo([UTF8] {cpp_type} text) => ([UTF8] {cpp_type} text) }}").count(
                        "Utf8View(text).validate()"
                    ),
                    2,
                )

    def test_utf8_strings_need_no_additional_validation(self) -> None:
        generated = self.generate("endpoint Test { echo([UTF8] String text) => ([UTF8] String text) }")
        self.assertNotIn("validate_utf8", generated)
        self.assertIn("pub text: ::libipc_rust::wire::Utf8String", generated)

    def test_utf8_requires_a_byte_string_type(self) -> None:
        for cpp_type in ["u32", "Utf16String", "IPC::File", "Optional<ByteString>", "Test::CustomType"]:
            with self.subTest(cpp_type=cpp_type):
                with self.assertRaisesRegex(RuntimeError, r"Cannot apply \[UTF8\] to Rust parameter 'value'"):
                    self.generate(f"endpoint Test {{ update([UTF8] {cpp_type} value) =| }}")


if __name__ == "__main__":
    unittest.main()
