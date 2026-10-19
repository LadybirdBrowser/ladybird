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
        self.assertIn(f"pub const ENDPOINT_MAGIC: u32 = {parse(ENDPOINT)[0].magic};", generated)

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
