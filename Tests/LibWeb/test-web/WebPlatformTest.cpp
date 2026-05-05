/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WebPlatformTest.h"
#include "Application.h"
#include "Fixture.h"

#include <AK/Array.h>
#include <AK/ByteBuffer.h>
#include <AK/Format.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/MappedFile.h>
#include <LibCore/Process.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWebView/Forward.h>
#include <LibWebView/Utilities.h>

namespace TestWeb {

static ErrorOr<void> collect_manifest_entries(JsonObject const&, Vector<Test>&, TestMode, StringView prefix = {});
static ErrorOr<void> write_result(Test const&);
static TestResult process_test_result(Test&);
static bool is_valid_test_extension(StringView test_name);

static constexpr StringView WPT_PATH_PREFIX = "WPT/wpt/"sv;

class WebPlatformTestFixture final : public Fixture {
public:
    virtual ErrorOr<void> setup(WebView::WebContentOptions&) override;
    virtual void teardown_impl() override;
    virtual StringView name() const override { return "WPTHttpTestServer"sv; }
    virtual bool is_running() const override { return m_process.has_value(); }

private:
    Optional<Core::Process> m_process;
};

NonnullOwnPtr<Fixture> create_web_platform_test_fixture() { return make<WebPlatformTestFixture>(); }

ErrorOr<void> WebPlatformTestFixture::setup(WebView::WebContentOptions&)
{
#ifndef AK_OS_WINDOWS
    Application& app = Application::the();
    ByteString const script_path = LexicalPath::join(app.test_root_path, "Fixtures"sv, "wpt-server.py"sv).string();
    ByteString const log_path = LexicalPath::join(app.test_root_path, "Fixtures"sv, "wpt-server.log"sv).string();
    Vector<ByteString> const arguments { script_path };

    auto can_connect = [](u16 port) { return !Core::TCPSocket::connect("127.0.0.1"sv, port).is_error(); };

    if (can_connect(8000) || can_connect(8443) || can_connect(9000)) {
        warnln("WPT server needs ports 8000, 8443, and 9000 free, but someone's already listening");
        return Error::from_string_literal("test-web: Could not start WPT server");
    }
    (void)Core::System::unlink(log_path);

    Core::ProcessSpawnOptions const process_options {
        .executable = app.python_executable_path,
        .search_for_executable_in_path = true,
        .arguments = arguments,
        .file_actions = {
            Core::FileAction::OpenFile { log_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate,
                STDERR_FILENO },
            Core::FileAction::DupFd { STDERR_FILENO, STDOUT_FILENO },
        }
    };
    m_process = TRY(Core::Process::spawn(process_options));

    for (size_t attempt = 0; attempt < 25; ++attempt) {
        if (ErrorOr<void> sleep_result = Core::System::sleep_ms(200); sleep_result.is_error()) {
            if (sleep_result.error().code() != EINTR)
                return sleep_result.release_error();
        }
        if (can_connect(8000) || can_connect(8443))
            return {};
    }
    warnln("Timed out starting WPT server, see {}", log_path);
#endif
    return Error::from_string_literal("test-web: Could not start WPT server");
}

void WebPlatformTestFixture::teardown_impl()
{
    if (!m_process.has_value())
        return;

#ifndef AK_OS_WINDOWS
    bool should_wait = true;
    if (ErrorOr<void> maybe_error = Core::System::kill(m_process->pid(), SIGINT); maybe_error.is_error()) {
        if (maybe_error.error().code() != ESRCH) {
            warnln("Failed to kill wpt server, error: {}", maybe_error.error());
            should_wait = false;
        }
    }
    if (should_wait) {
        if (ErrorOr<int> maybe_code = m_process->wait_for_termination(); maybe_code.is_error())
            warnln("Failed to kill wpt server, error: {}", maybe_code.error());
    }
    m_process = {};
#endif
}

ErrorOr<void> collect_wpt_tests(Vector<Test>& tests)
{
    Application& app = Application::the();
    if (app.wpt_filters.is_empty())
        return {};

    Vector<ByteString> wpt_globs;
    for (ByteString const& glob : app.wpt_filters) {
        wpt_globs.append(ByteString::formatted("*{}*", glob));
    }
    ByteString const manifest_path = LexicalPath::join(app.wpt_path, "MANIFEST.json"sv).string();
    NonnullOwnPtr<Core::MappedFile> manifest_file = TRY(Core::MappedFile::map(manifest_path));
    JsonValue manifest = TRY(JsonValue::from_string(StringView { manifest_file->bytes() }));

    if (!manifest.is_object() || !manifest.as_object().has_object("items"sv))
        return Error::from_string_literal("Bad WPT manifest");

    JsonObject const& items = manifest.as_object().get_object("items"sv).value();

    if (Optional<JsonObject const&> testharness = items.get_object("testharness"sv); testharness.has_value())
        TRY(collect_manifest_entries(*testharness, tests, TestMode::Text));
    if (Optional<JsonObject const&> reftest = items.get_object("reftest"sv); reftest.has_value())
        TRY(collect_manifest_entries(*reftest, tests, TestMode::Ref));
    if (Optional<JsonObject const&> crashtest = items.get_object("crashtest"sv); crashtest.has_value())
        TRY(collect_manifest_entries(*crashtest, tests, TestMode::Crash));

    tests.remove_all_matching([&](auto const& test) {
        if (!test.is_wpt_test)
            return false;
        auto const test_relative_path = test.relative_path.replace("\\"sv, "/"sv);
        return !any_of(wpt_globs, [&](auto const& glob) { return test_relative_path.matches(glob, CaseSensitivity::CaseSensitive); });
    });
    return {};
}

ErrorOr<TestResult> on_wpt_test_result(Test& test, URL::URL const& url)
{
    if (process_test_result(test) == TestResult::Fail) {
        TRY(write_result(test));
        if (!Application::the().quiet
            && Application::the().verbosity >= Application::VERBOSITY_LEVEL_LOG_TEST_OUTPUT
            && !test.text.is_empty()) {
            outln("WPT Test failed: {} {}", url, test.text);
        }
        return TestResult::Fail;
    }
    return TestResult::Pass;
}

URL::URL wpt_url(StringView relative_path)
{
    StringView request_path = relative_path.starts_with(WPT_PATH_PREFIX)
        ? relative_path.substring_view(WPT_PATH_PREFIX.length())
        : relative_path;
    u16 const port = [request_path] {
        if (request_path.contains(".h2."sv))
            return 9000;
        if (request_path.contains(".https."sv))
            return 8443;
        return 8000;
    }();
    StringView const scheme = port == 8000 ? "http"sv : "https"sv;
    Optional<URL::URL> url = URL::Parser::basic_parse(ByteString::formatted("{}://web-platform.test:{}/{}", scheme, port, request_path));
    VERIFY(url.has_value());
    return url.release_value();
}

static ErrorOr<void> collect_manifest_entries(
    JsonObject const& object,
    Vector<Test>& tests, TestMode mode,
    StringView prefix)
{
    Application& app = Application::the();
    TRY(object.try_for_each_member([&](String const& key, JsonValue const& value) -> ErrorOr<void> {
        ByteString const current_path = prefix.is_empty() ? key.to_byte_string() : ByteString::formatted("{}/{}", prefix, key);

        if (value.is_object())
            return collect_manifest_entries(value.as_object(), tests, mode, current_path);

        if (!value.is_array())
            return {};

        ByteString const input_path = TRY(FileSystem::real_path(LexicalPath::join(app.wpt_path, current_path).string()));
        JsonArray const& generated_tests = value.as_array();
        for (size_t i = 1; i < generated_tests.size(); ++i) {
            JsonValue const& generated_test = generated_tests[i];
            if (!generated_test.is_array())
                continue;

            JsonArray const& generated_test_parts = generated_test.as_array();
            ByteString wpt_path = current_path;
            if (!generated_test_parts.is_empty() && generated_test_parts[0].is_string())
                wpt_path = generated_test_parts[0].as_string().to_byte_string();

            if (!is_valid_test_extension(wpt_path))
                continue;

            StringView const sub_path = wpt_path.starts_with('/') ? StringView { wpt_path }.substring_view(1) : StringView { wpt_path };
            ByteString const relative_path = ByteString::formatted("{}{}", WPT_PATH_PREFIX, sub_path);
            ByteString const safe_path = relative_path.replace("?"sv, "@"sv);
            Test test { mode, input_path, {}, relative_path, safe_path };
            test.is_wpt_test = true;
            tests.append(move(test));
        }

        return {};
    }));
    return {};
}

static ErrorOr<void> write_result(Test const& test)
{
    ByteString const base_path = TRY(prepare_output_path(test));
    ByteString const html_path = ByteString::formatted("{}.wpt.html", base_path);
    NonnullOwnPtr<Core::File> html_file = TRY(Core::File::open(html_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));

    TRY(html_file->write_until_depleted(R"html(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<style>
body { margin: 0; background: #0d1117; color: #c9d1d9; }
pre { margin: 0; padding: 16px; font-family: ui-monospace, monospace; font-size: 12px; line-height: 1.5; white-space: pre-wrap; }
</style>
</head>
<body><pre>)html"sv));
    TRY(html_file->write_formatted("{}", escape_html_entities(test.text)));
    TRY(html_file->write_until_depleted("</pre></body></html>"sv));

    return {};
}

static TestResult process_test_result(Test& test)
{
    bool harness_passed = false;

    for (StringView raw_line : test.text.bytes_as_string_view().lines()) {
        StringView line = raw_line.trim_whitespace();
        if (line.is_empty())
            continue;

        if (line.starts_with("Harness status:"sv)) {
            harness_passed = line.ends_with("OK"sv);
            continue;
        }
        Optional<size_t> tab_index = line.find('\t');
        if (!tab_index.has_value())
            continue;

        test.subtest_count = test.subtest_count.value_or(0) + 1;
        StringView status = line.substring_view(0, *tab_index);
        if (status == "Pass"sv)
            test.subtests_passed++;
    }

    return (!harness_passed || test.subtests_passed < test.subtest_count.value_or(0)) ? TestResult::Fail : TestResult::Pass;
}

static bool is_valid_test_extension(StringView test_name)
{
    static constexpr Array<StringView, 5> VALID { ".htm"sv, ".html"sv, ".svg"sv, ".xhtml"sv, ".xht"sv };
    return AK::any_of(VALID, [&](StringView suffix) { return test_name.ends_with(suffix); });
}

} // namespace TestWeb
