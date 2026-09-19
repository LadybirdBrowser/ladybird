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


def generate(contents: str) -> str:
    out = io.StringIO()
    build(out, parse(contents))
    return out.getvalue()


class TestIPCGenerator(unittest.TestCase):
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
    NEVER_INLINE ErrorOr<OwnPtr<IPC::MessageBuffer>> handle_ping()
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
    virtual void page_changed(Web::PageId page_id, u32 revision) override
    {
        if (auto* stub = page_stub(page_id))
            stub->page_changed(revision);
    }
""",
            generated,
        )
        self.assertIn(
            """\
    virtual void page_acknowledged(Web::PageId page_id) override
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
    void async_page_changed(u32 revision) const
    {
        if (auto* connection = derived().routed_connection())
            connection->async_page_changed(derived().routed_page_id(), revision);
    }
""",
            generated,
        )
        self.assertIn("    HTTP::Cookie::VersionedCookie page_wants_cookie() const\n", generated)
        self.assertNotIn("async_ping", generated.split("class TestClientPageProxy {")[1])

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
