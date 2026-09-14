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


if __name__ == "__main__":
    unittest.main()
