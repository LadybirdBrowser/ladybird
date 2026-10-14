import io
import os
import sys
import unittest

from pathlib import Path

sys.path.insert(0, str(Path(os.environ["LADYBIRD_SOURCE_DIR"]) / "Meta"))

from Generators.generate_ipc_definitions import build
from Generators.generate_ipc_definitions import parse

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


if __name__ == "__main__":
    unittest.main()
