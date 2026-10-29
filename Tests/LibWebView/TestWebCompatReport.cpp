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
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/WebCompatReport.h>

using namespace ReportServerTest;
using WebView::ReportSubmission;
using WebView::WebCompatReport;

namespace {

constexpr auto page_dumps = "Layout tree:\nViewport <#document>\n\nDisplay list:\nFillRect\n"sv;
constexpr auto screenshot_png = "\x89PNG\r\n\x1a\nnot really a PNG"sv;

struct Outcome {
    bool sent { false };
    Optional<ReportSubmission::Failure> failure;
    String reason;

    bool is_finished() const { return sent || failure.has_value(); }
};

void track(WebCompatReport& report, Outcome& outcome)
{
    outcome = {};
    report.on_sent = [&outcome] { outcome.sent = true; };
    report.on_failed = [&outcome](auto failure, String const& reason) {
        outcome.failure = failure;
        outcome.reason = reason;
    };
}

WebCompatReport::Page test_page()
{
    return {
        .url = URL::Parser::basic_parse("https://example.com/broken?query"sv).release_value(),
        .user_agent = "Mozilla/5.0 Ladybird/1.0"_string,
        .viewport_size = { 1280, 720 },
        .device_pixel_ratio = 2,
        .zoom_level = 1.1,
    };
}

void verify_validation()
{
    VERIFY(WebCompatReport::validate(""sv) == "Describe what is wrong with this website."sv);
    VERIFY(WebCompatReport::validate(" \n "sv) == "Describe what is wrong with this website."sv);
    VERIFY(!WebCompatReport::validate("The menu does not open"sv).has_value());

    auto long_description = ByteString::repeated('a', WebCompatReport::maximum_description_bytes + 1);
    VERIFY(WebCompatReport::validate(long_description) == "This description is too long to include in a report."sv);

    WebCompatReport report { test_page() };
    VERIFY(report.send(""_string, {}, WebCompatReport::IncludePageDumps::No) == "Describe what is wrong with this website."sv);
    VERIFY(!report.is_sending());
}

void verify_diagnostics()
{
    WebCompatReport report { test_page() };
    VERIFY(!report.has_page_dumps());

    auto diagnostics = report.diagnostics(WebCompatReport::IncludePageDumps::Yes);
    VERIFY(diagnostics.starts_with("Ladybird web compatibility report, format 1\n"sv));
    VERIFY(diagnostics.contains("\nURL: https://example.com/broken?query\n"sv));
    VERIFY(diagnostics.contains("\nUser agent: Mozilla/5.0 Ladybird/1.0\n"sv));
    VERIFY(diagnostics.contains("\nViewport size: 1280x720\n"sv));
    VERIFY(diagnostics.contains("\nDevice pixel ratio: 2\n"sv));
    VERIFY(diagnostics.contains("\nZoom level: 110%\n"sv));
    VERIFY(diagnostics.contains("\nGit commit: "sv));

    report.set_page_dumps(String::from_utf8_without_validation(page_dumps.bytes()));
    VERIFY(report.has_page_dumps());
    VERIFY(report.diagnostics(WebCompatReport::IncludePageDumps::Yes).ends_with(ByteString::formatted("\n\n{}", page_dumps)));
    VERIFY(report.diagnostics(WebCompatReport::IncludePageDumps::No) == diagnostics);

    // Dumps over the limit are cut where a character starts, and say so.
    StringBuilder builder;
    builder.append(ByteString::repeated('a', WebCompatReport::maximum_page_dumps_bytes - 1));
    builder.append("é and more"sv);
    report.set_page_dumps(MUST(builder.to_string()));
    auto truncated = report.diagnostics(WebCompatReport::IncludePageDumps::Yes);
    VERIFY(truncated.ends_with("a\n(Truncated)\n"sv));
    VERIFY(!truncated.contains("é"sv));

    auto details = report.details();
    VERIFY(details[0].label == "Website"sv);
    VERIFY(details[0].value == "https://example.com/broken?query"sv);
    VERIFY(details[1].label == "User agent"sv);
    VERIFY(details[1].is_code);
}

}

TEST_CASE(web_compat_report)
{
    Test::HeadlessTestEnvironment environment { "TestWebCompatReport"sv };
    auto const& test_directory = environment.directory();

    MUST(Core::Environment::set("XDG_CONFIG_HOME"sv, LexicalPath::join(test_directory, "config"sv).string(), Core::Environment::Overwrite::Yes));

    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());

    verify_validation();
    verify_diagnostics();

    ReportServer server;
    MUST(Core::Environment::set("LADYBIRD_REPORTS_ENDPOINT"sv, server.endpoint(), Core::Environment::Overwrite::Yes));

    // A report with a screenshot and the page's dumps carries both, next to the fields that tell pages apart.
    WebCompatReport report { test_page() };
    report.set_page_dumps(String::from_utf8_without_validation(page_dumps.bytes()));
    Outcome outcome;
    track(report, outcome);
    server.respond_with(challenge("first"sv));
    server.respond_with({ 200, R"({"receipt":"receipt-1"})", {} });

    VERIFY(!report.send("The menu does not open"_string, MUST(ByteBuffer::copy(screenshot_png.bytes())),
                      WebCompatReport::IncludePageDumps::Yes)
            .has_value());
    VERIFY(report.is_sending());
    Core::EventLoop::current().spin_until([&] { return outcome.is_finished(); });
    VERIFY(outcome.sent);
    VERIFY(server.has_answered_everything());
    VERIFY(server.requests.size() == 2);

    auto const& upload = server.requests[1];
    VERIFY(upload.path == "/api/v1/reports"sv);
    auto manifest_text = multipart_part(upload.body, "manifest"sv);
    auto diagnostics = multipart_part(upload.body, "diagnostics"sv);
    VERIFY(diagnostics == report.diagnostics(WebCompatReport::IncludePageDumps::Yes));
    VERIFY(diagnostics.ends_with(page_dumps));
    VERIFY(multipart_part(upload.body, "screenshot"sv) == screenshot_png);
    VERIFY(upload.body.contains("Content-Disposition: form-data; name=\"screenshot\"; filename=\"screenshot.png\"\r\nContent-Type: image/png\r\n"sv));

    auto manifest = JsonValue::from_string(manifest_text).release_value().as_object();
    VERIFY(manifest.get_string("kind"sv) == "web_compat"sv);
    VERIFY(manifest.get_string("client_version"sv) == "1.0"sv);
    VERIFY(manifest_field(manifest, "description"sv)->as_string() == "The menu does not open"sv);
    VERIFY(manifest_field(manifest, "url"sv)->as_string() == "https://example.com/broken?query"sv);
    VERIFY(manifest_field(manifest, "user_agent"sv)->as_string() == "Mozilla/5.0 Ladybird/1.0"sv);
    VERIFY(manifest_field(manifest, "viewport_size"sv)->as_string() == "1280x720"sv);
    VERIFY(manifest_field(manifest, "device_pixel_ratio"sv)->get_double_with_precision_loss() == 2.);
    VERIFY(manifest_field(manifest, "zoom_level"sv)->get_double_with_precision_loss() == 1.1);
    VERIFY(manifest_field(manifest, "platform"sv).has_value());
    VERIFY(manifest_field(manifest, "git_commit"sv).has_value());

    auto attachments = manifest.get_array("attachments"sv).value();
    VERIFY(attachments.size() == 2);
    auto const& described_diagnostics = attachments.at(0).as_object();
    VERIFY(described_diagnostics.get_string("id"sv) == "diagnostics"sv);
    VERIFY(described_diagnostics.get_string("name"sv) == "webcompat-diagnostics.txt"sv);
    VERIFY(described_diagnostics.get_string("media_type"sv) == "text/plain"sv);
    VERIFY(described_diagnostics.get_string("sha256"sv)->bytes_as_string_view() == sha256_hex(diagnostics));
    auto const& described_screenshot = attachments.at(1).as_object();
    VERIFY(described_screenshot.get_string("id"sv) == "screenshot"sv);
    VERIFY(described_screenshot.get_string("media_type"sv) == "image/png"sv);
    VERIFY(described_screenshot.get_integer<size_t>("size"sv) == screenshot_png.length());
    VERIFY(described_screenshot.get_string("sha256"sv)->bytes_as_string_view() == sha256_hex(screenshot_png));

    // Without a screenshot or dumps, only the diagnostics go along. Trying again after a failure sends the same
    // manifest, submission ID included.
    server.requests.clear();
    WebCompatReport plain_report { test_page() };
    plain_report.set_page_dumps(String::from_utf8_without_validation(page_dumps.bytes()));
    track(plain_report, outcome);
    server.respond_with({ 400, "{}", {} });
    VERIFY(!plain_report.send("Text overlaps"_string, {}, WebCompatReport::IncludePageDumps::No).has_value());
    Core::EventLoop::current().spin_until([&] { return outcome.is_finished(); });
    VERIFY(outcome.failure == ReportSubmission::Failure::Sending);
    VERIFY(outcome.reason == "Report server returned 400 while requesting a challenge."sv);
    VERIFY(!plain_report.is_sending());

    track(plain_report, outcome);
    server.respond_with(challenge("second"sv));
    server.respond_with({ 200, R"({"receipt":"receipt-2"})", {} });
    plain_report.retry();
    Core::EventLoop::current().spin_until([&] { return outcome.is_finished(); });
    VERIFY(outcome.sent);
    VERIFY(server.requests.size() == 3);
    VERIFY(server.requests[0].body == server.requests[1].body);

    auto const& plain_upload = server.requests[2];
    VERIFY(!plain_upload.body.contains("name=\"screenshot\""sv));
    VERIFY(!multipart_part(plain_upload.body, "diagnostics"sv).contains("Layout tree:"sv));
    manifest = JsonValue::from_string(multipart_part(plain_upload.body, "manifest"sv)).release_value().as_object();
    VERIFY(manifest.get_array("attachments"sv)->size() == 1);

    // A canceled report stops sending and hears nothing more, not even about the request it was waiting on.
    server.requests.clear();
    WebCompatReport canceled_report { test_page() };
    track(canceled_report, outcome);
    VERIFY(!canceled_report.send("Nothing happens"_string, {}, WebCompatReport::IncludePageDumps::No).has_value());
    Core::EventLoop::current().spin_until([&] { return server.requests.size() == 1; });
    canceled_report.cancel_sending();
    VERIFY(!canceled_report.is_sending());

    auto has_waited = false;
    auto wait = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(200), [&] { has_waited = true; });
    wait->start();
    Core::EventLoop::current().spin_until([&] { return has_waited; });
    VERIFY(!outcome.is_finished());
    VERIFY(server.requests.size() == 1);

    outln("PASS: Web compatibility reports reach the report server in the format it expects");
}
