/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include "ReportServer.h"
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/CrashReportReview.h>

using namespace ReportServerTest;

namespace {

constexpr auto report_text = "Ladybird crash report, format 1\n"
                             "Process: WebContent\n"
                             "Version: 1.0\n"
                             "Build configuration: Release\n"
                             "Termination signal: SIGSEGV\n"
                             "Termination signal number: 11\n"
                             "\n"
                             "Native stack (binary build ID, object address):\n"
                             "#0 abcdef 0x1234 Web::Page::crash()\n"sv;

struct Outcome {
    bool sent { false };
    Optional<WebView::ReportSubmission::Failure> failure;
    String reason;
    Vector<u32> retry_delays;

    bool is_finished() const { return sent || failure.has_value(); }
};

void track(WebView::CrashReportReview& review, Outcome& outcome)
{
    outcome = {};
    review.on_retry = [&outcome](String const&, u32 retry_number, u32 maximum_retries, u32 delay_seconds) {
        VERIFY(retry_number == outcome.retry_delays.size() + 1);
        VERIFY(maximum_retries == WebView::ReportSubmission::maximum_attempts - 1);
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

TEST_CASE(crash_report_submission)
{
    Test::HeadlessTestEnvironment environment { "TestCrashReportSubmission"sv };
    auto const& test_directory = environment.directory();

    MUST(Core::Environment::set("XDG_CONFIG_HOME"sv, LexicalPath::join(test_directory, "config"sv).string(), Core::Environment::Overwrite::Yes));

    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());

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
    VERIFY(outcome.failure == WebView::ReportSubmission::Failure::Sending);
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

    // Other choices than those of a failed or canceled attempt make a new submission, which carries only those.
    server.requests.clear();
    track(review, outcome);
    name = write_report(reports_directory, "2026-10-01T13-30-00Z-WebContent-abc123.txt"sv);
    MUST(review.open(name));
    server.respond_with({ 400, "{}", {} });
    VERIFY(!review.send("I clicked a link"_string, "https://example.com/"_string).has_value());
    Core::EventLoop::current().spin_until([&] { return outcome.is_finished(); });
    VERIFY(outcome.failure == WebView::ReportSubmission::Failure::Sending);

    track(review, outcome);
    server.respond_with(challenge("changed"sv));
    server.respond_with({ 200, R"({"receipt":"receipt-changed"})", {} });
    VERIFY(!review.send("I opened a menu"_string, {}).has_value());
    Core::EventLoop::current().spin_until([&] { return outcome.is_finished(); });
    VERIFY(outcome.sent);
    VERIFY(server.requests.size() == 3);
    VERIFY(server.requests[0].body != server.requests[1].body);
    manifest = JsonValue::from_string(multipart_part(server.requests[2].body, "manifest"sv)).release_value().as_object();
    VERIFY(manifest_field(manifest, "description"sv)->as_string() == "I opened a menu"sv);
    VERIFY(!manifest_field(manifest, "url"sv).has_value());
    VERIFY(manifest.get_string("submission_id"sv).value() != submission_id);

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
}
