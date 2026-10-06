/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Hex.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Socket.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/TCPServer.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibFileSystem/FileSystem.h>
#include <LibMain/Main.h>
#include <LibWebView/Application.h>
#include <LibWebView/CrashReportReview.h>

namespace {

class TestApplication : public WebView::Application {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions&) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
};

constexpr auto report_text = "Ladybird crash report, format 1\n"
                             "Process: WebContent\n"
                             "Version: 1.0\n"
                             "Build configuration: Release\n"
                             "Termination signal: SIGSEGV\n"
                             "Termination signal number: 11\n"
                             "\n"
                             "Native stack (binary build ID, object address):\n"
                             "#0 abcdef 0x1234 Web::Page::crash()\n"sv;

struct ReceivedRequest {
    ByteString path;
    HashMap<ByteString, ByteString> headers;
    ByteString body;
};

struct Response {
    u32 status { 200 };
    ByteString body;
    ByteString extra_headers;
};

Response challenge(StringView token)
{
    return { 200, ByteString::formatted(R"({{"algorithm":"sha256-seed-nonce-le-v1","token":"{}","expected_work":1}})", token), {} };
}

// A report server on the loopback interface that answers each request with the next of the responses it was given.
class ReportServer {
public:
    ReportServer()
    {
        m_server = MUST(Core::TCPServer::try_create());
        MUST(m_server->listen(IPv4Address { 127, 0, 0, 1 }, 0));
        m_server->on_ready_to_accept = [this] {
            auto socket = MUST(m_server->accept());
            MUST(socket->set_blocking(false));
            m_connections.append(make<Connection>(move(socket)));
            auto& connection = *m_connections.last();
            connection.socket->on_ready_to_read = [this, &connection] { read_from(connection); };
        };
    }

    ByteString endpoint() const { return ByteString::formatted("http://127.0.0.1:{}", *m_server->local_port()); }

    void respond_with(Response response) { m_responses.append(move(response)); }
    bool has_answered_everything() const { return m_responses.is_empty(); }

    Vector<ReceivedRequest> requests;

private:
    struct Connection {
        AK_ALLOC_WITH_KMALLOC;

        NonnullOwnPtr<Core::TCPSocket> socket;
        ByteBuffer data {};
        bool sent_continue { false };
    };

    void read_from(Connection& connection)
    {
        auto buffer = MUST(ByteBuffer::create_uninitialized(64 * KiB));
        auto received = connection.socket->read_some(buffer);
        if (received.is_error() || received.value().is_empty()) {
            connection.socket->on_ready_to_read = nullptr;
            return;
        }
        connection.data.append(received.value());

        auto data = StringView { connection.data.bytes() };
        auto header_end = data.find("\r\n\r\n"sv);
        if (!header_end.has_value())
            return;

        ReceivedRequest request;
        auto lines = data.substring_view(0, *header_end).split_view("\r\n"sv);
        request.path = lines[0].split_view(' ')[1];
        for (size_t i = 1; i < lines.size(); ++i) {
            auto separator = lines[i].find(':').value();
            request.headers.set(ByteString { lines[i].substring_view(0, separator) }.to_lowercase(),
                lines[i].substring_view(separator + 1).trim_whitespace());
        }

        auto content_length = request.headers.get("content-length"sv).value_or("0"sv).to_number<size_t>().value();
        auto body = data.substring_view(*header_end + 4);
        if (body.length() < content_length) {
            if (request.headers.get("expect"sv) == "100-continue"sv && !connection.sent_continue) {
                connection.sent_continue = true;
                MUST(connection.socket->write_until_depleted("HTTP/1.1 100 Continue\r\n\r\n"sv.bytes()));
            }
            return;
        }
        request.body = body.substring_view(0, content_length);
        requests.append(move(request));

        connection.socket->on_ready_to_read = nullptr;
        auto response = m_responses.take_first();
        MUST(connection.socket->set_blocking(true));
        auto response_text = ByteString::formatted(
            "HTTP/1.1 {} Status\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n{}\r\n{}",
            response.status, response.body.length(), response.extra_headers, response.body);
        MUST(connection.socket->write_until_depleted(response_text.bytes()));
        connection.socket->close();
    }

    RefPtr<Core::TCPServer> m_server;
    Vector<NonnullOwnPtr<Connection>> m_connections;
    Vector<Response> m_responses;
};

ByteString sha256_hex(StringView text)
{
    return encode_hex(Crypto::Hash::SHA256::hash(text.bytes()).bytes());
}

// The content of the multipart part with the given name.
StringView multipart_part(StringView body, StringView name)
{
    auto disposition = ByteString::formatted("Content-Disposition: form-data; name=\"{}\"", name);
    auto start = body.find(disposition).value();
    auto content_start = body.find("\r\n\r\n"sv, start).value() + 4;
    auto content_end = body.find("\r\n--"sv, content_start).value();
    return body.substring_view(content_start, content_end - content_start);
}

Optional<JsonValue> manifest_field(JsonObject const& manifest, StringView key)
{
    Optional<JsonValue> value;
    manifest.get_array("fields"sv)->for_each([&](JsonValue const& field) {
        if (field.as_object().get_string("key"sv) == key)
            value = field.as_object().get_object("value"sv)->get("value"sv).value();
    });
    return value;
}

struct Outcome {
    bool sent { false };
    Optional<WebView::CrashReportSubmission::Failure> failure;
    String reason;
    Vector<u32> retry_delays;

    bool is_finished() const { return sent || failure.has_value(); }
};

void track(WebView::CrashReportReview& review, Outcome& outcome)
{
    outcome = {};
    review.on_retry = [&outcome](String const&, u32 retry_number, u32 maximum_retries, u32 delay_seconds) {
        VERIFY(retry_number == outcome.retry_delays.size() + 1);
        VERIFY(maximum_retries == WebView::CrashReportSubmission::maximum_attempts - 1);
        outcome.retry_delays.append(delay_seconds);
    };
    review.on_sent = [&outcome] { outcome.sent = true; };
    review.on_failed = [&outcome](auto failure, String const& reason) {
        outcome.failure = failure;
        outcome.reason = reason;
    };
}

ByteString write_report(ByteString const& directory, StringView name)
{
    auto reports = MUST(Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes));
    auto file = MUST(reports.open(name, Core::File::OpenMode::Write));
    MUST(file->write_until_depleted(report_text.bytes()));
    return name;
}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_directory = ByteString::formatted("{}/Ladybird-TestCrashReportSubmission-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(test_directory, Core::Directory::CreateDirectories::Yes));
    auto cleanup_test_directory = ScopeGuard([&] {
        MUST(FileSystem::remove(test_directory, FileSystem::RecursionMode::Allowed));
    });
    MUST(Core::Environment::set("XDG_CONFIG_HOME"sv, LexicalPath::join(test_directory, "config"sv).string(), Core::Environment::Overwrite::Yes));

#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(TestApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(TestApplication::create(arguments, OptionalNone {}));
#endif

    ReportServer server;
    MUST(Core::Environment::set("LADYBIRD_REPORTS_ENDPOINT"sv, server.endpoint(), Core::Environment::Overwrite::Yes));

    auto reports_directory = LexicalPath::join(test_directory, "CrashReports"sv).string();
    WebView::CrashReportStore store { reports_directory };
    WebView::CrashReportReview review { store };
    Outcome outcome;
    track(review, outcome);

    // A server that is briefly unavailable, or asks for time, gets another attempt after the delay it asks for.
    auto name = write_report(reports_directory, "2026-10-01T12-00-00Z-WebContent-abc123.txt"sv);
    MUST(review.open(name));
    server.respond_with({ 503, "{}", {} });
    server.respond_with(challenge("first"sv));
    server.respond_with({ 429, "{}", "Retry-After: 4\r\n" });
    server.respond_with(challenge("second"sv));
    server.respond_with({ 200, R"({"receipt":"receipt-1"})", {} });

    VERIFY(!review.send("I clicked a link"_string, "https://example.com/"_string).has_value());
    Core::EventLoop::current().spin_until([&] { return outcome.is_finished(); });
    VERIFY(outcome.sent);
    VERIFY(server.has_answered_everything());
    VERIFY((outcome.retry_delays == Vector<u32> { 3, 4 }));

    VERIFY(server.requests.size() == 5);
    VERIFY(server.requests[0].path == "/api/v1/challenges"sv);
    VERIFY(server.requests[2].path == "/api/v1/reports"sv);
    VERIFY(server.requests[4].path == "/api/v1/reports"sv);

    auto const& upload = server.requests[4];
    VERIFY(upload.headers.get("x-ladybird-challenge"sv) == "second"sv);
    VERIFY(upload.headers.get("x-ladybird-nonce"sv) == "0"sv);

    auto content_type = upload.headers.get("content-type"sv).value();
    VERIFY(content_type.starts_with("multipart/form-data; boundary="sv));
    auto manifest_text = multipart_part(upload.body, "manifest"sv);
    VERIFY(multipart_part(upload.body, "diagnostics"sv) == report_text);

    // Each attempt sends the same manifest, so the server can tell a repeated submission from a new one.
    auto challenge_body = JsonValue::from_string(server.requests[3].body).release_value();
    VERIFY(challenge_body.as_object().get_string("manifest_digest"sv)->bytes_as_string_view() == sha256_hex(manifest_text));
    VERIFY(server.requests[0].body == server.requests[3].body);

    auto manifest = JsonValue::from_string(manifest_text).release_value().as_object();
    VERIFY(manifest.get_integer<u32>("protocol"sv) == 1u);
    VERIFY(manifest.get_string("kind"sv) == "crash"sv);
    VERIFY(manifest.get_string("client_version"sv) == "1.0"sv);
    VERIFY(manifest.get_string("build"sv) == "Release"sv);
    auto submission_id = manifest.get_string("submission_id"sv).value();
    VERIFY(submission_id.bytes().size() == 36);
    VERIFY(submission_id.bytes_as_string_view()[14] == '4');

    VERIFY(manifest_field(manifest, "description"sv)->as_string() == "I clicked a link"sv);
    VERIFY(manifest_field(manifest, "url"sv)->as_string() == "https://example.com/"sv);
    VERIFY(manifest_field(manifest, "signal"sv)->as_string() == "SIGSEGV"sv);
    VERIFY(manifest_field(manifest, "process"sv)->as_string() == "WebContent"sv);

    auto attachments = manifest.get_array("attachments"sv).value();
    VERIFY(attachments.size() == 1);
    auto const& attachment = attachments.at(0).as_object();
    VERIFY(attachment.get_string("id"sv) == "diagnostics"sv);
    VERIFY(attachment.get_string("name"sv) == "crash-diagnostics.txt"sv);
    VERIFY(attachment.get_integer<size_t>("size"sv) == report_text.length());
    VERIFY(attachment.get_string("sha256"sv)->bytes_as_string_view() == sha256_hex(report_text));

    // Once the server has a report, the local copy has done its job.
    VERIFY(!FileSystem::exists(LexicalPath::join(reports_directory, "Seen"sv, name).string()));
    VERIFY(!store.has_pending_reports());

    // A request the server rejects would be rejected again, so it ends the submission until the user tries again.
    server.requests.clear();
    track(review, outcome);
    name = write_report(reports_directory, "2026-10-01T13-00-00Z-WebContent-abc123.txt"sv);
    MUST(review.open(name));
    server.respond_with({ 400, "{}", {} });

    VERIFY(!review.send(""_string, {}).has_value());
    Core::EventLoop::current().spin_until([&] { return outcome.is_finished(); });
    VERIFY(outcome.failure == WebView::CrashReportSubmission::Failure::Sending);
    VERIFY(outcome.reason == "Report server returned 400 while requesting a challenge."sv);
    VERIFY(outcome.retry_delays.is_empty());
    VERIFY(server.requests.size() == 1);

    // Trying again sends the manifest that was prepared the first time, submission ID included.
    track(review, outcome);
    server.respond_with(challenge("third"sv));
    server.respond_with({ 200, R"({"receipt":"receipt-2"})", {} });
    VERIFY(!review.send(""_string, {}).has_value());
    Core::EventLoop::current().spin_until([&] { return outcome.is_finished(); });
    VERIFY(outcome.sent);
    VERIFY(server.requests.size() == 3);
    VERIFY(server.requests[0].body == server.requests[1].body);
    manifest = JsonValue::from_string(multipart_part(server.requests[2].body, "manifest"sv)).release_value().as_object();
    VERIFY(!manifest_field(manifest, "description"sv).has_value());
    VERIFY(!manifest_field(manifest, "url"sv).has_value());

    // A report on its way keeps going when the screen that showed its review goes away, along with the store it was
    // given.
    server.requests.clear();
    name = write_report(reports_directory, "2026-10-01T14-00-00Z-Compositor-abc123.txt"sv);
    {
        WebView::CrashReportStore departing_store { reports_directory };
        WebView::CrashReportReview departing_review { departing_store };
        MUST(departing_review.open(name));
        server.respond_with(challenge("fourth"sv));
        server.respond_with({ 200, R"({"receipt":"receipt-3"})", {} });
        VERIFY(!departing_review.send(""_string, {}).has_value());
    }
    Core::EventLoop::current().spin_until([&] { return server.has_answered_everything() && !FileSystem::exists(LexicalPath::join(reports_directory, "Seen"sv, name).string()); });
    VERIFY(server.requests.size() == 2);
    VERIFY(server.requests[1].path == "/api/v1/reports"sv);

    outln("PASS: Crash reports reach the report server in the format it expects");
    return 0;
}
