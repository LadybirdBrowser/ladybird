/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/CrashReportReview.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

using WebView::CrashReportReview;
using WebView::CrashReportSubmission;

static constexpr auto report_text = "Ladybird crash report, format 1\n"
                                    "Process: WebContent\n"
                                    "Crashed at: 2026-10-01T12:34:56Z\n"
                                    "Version: 1.0\n"
                                    "Platform: macOS 26.0\n"
                                    "Architecture: arm64\n"
                                    "Git commit: 9458821e43baaede478000e6e4ab472cc8b98d92\n"
                                    "Build options: BUILD_SHARED_LIBS=ON; ENABLE_FUZZERS=OFF\n"
                                    "Verification failed: value != 0\n"
                                    "Termination signal: SIGTRAP\n"
                                    "Termination signal number: 5\n"
                                    "Captured signal: SIGSEGV\n"
                                    "Captured signal number: 11\n"
                                    "\n"
                                    "Native stack (binary build ID, object address):\n"
                                    "#0 abcdef 0x1234 Web::Page::crash()\n"
                                    "#1 abcdef 0x5678\n"
                                    "#2 Unavailable\n"sv;

static ByteString test_directory()
{
    auto name = ByteString::formatted("test-crash-report-review-{}", getpid());
    return LexicalPath::join(Core::StandardPaths::tempfile_directory(), name).string();
}

static void cleanup()
{
    if (FileSystem::exists(test_directory()))
        MUST(FileSystem::remove(test_directory(), FileSystem::RecursionMode::Allowed));
}

static ByteString write_report(StringView timestamp, StringView text = report_text)
{
    auto name = ByteString::formatted("{}-WebContent-abc123.txt", timestamp);
    auto directory = MUST(Core::Directory::create(test_directory(), Core::Directory::CreateDirectories::Yes));
    auto file = MUST(directory.open(name, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    MUST(file->write_until_depleted(text.bytes()));
    return name;
}

static bool is_pending(StringView name)
{
    return FileSystem::exists(LexicalPath::join(test_directory(), name).string());
}

TEST_CASE(opening_a_report_shows_what_crashed)
{
    cleanup();
    ScopeGuard guard = cleanup;

    // The crash time is shown in local time, here two hours ahead of UTC.
    VERIFY(setenv("TZ", "XYZ-2", 1) == 0);
    tzset();

    auto name = write_report("2026-10-01T12-34-56Z"sv);
    WebView::CrashReportStore store { test_directory() };
    CrashReportReview review { store };
    auto report = MUST(review.open(name));

    EXPECT_EQ(report.name, name);

    struct ExpectedField {
        StringView label;
        StringView value;
        bool is_code;
    };
    // A signal's name and number make up one field. What only helps to rebuild the crashed binary, such as its build
    // options, and what repeats another field, such as the captured signal, are left to the report's text.
    Array<ExpectedField, 8> expected_fields { {
        { "Crash date"sv, "2026-10-01 14:34:56"sv, false },
        { "Verification failed"sv, "value != 0"sv, true },
        { "Termination signal"sv, "SIGTRAP (5)"sv, false },
        { "Ladybird version"sv, "1.0"sv, true },
        { "Platform"sv, "macOS 26.0"sv, false },
        { "Process"sv, "WebContent"sv, false },
        { "Architecture"sv, "arm64"sv, false },
        { "Git commit"sv, "9458821e43baaede478000e6e4ab472cc8b98d92"sv, true },
    } };
    EXPECT_EQ(report.fields.size(), expected_fields.size());
    for (size_t i = 0; i < min(report.fields.size(), expected_fields.size()); ++i) {
        EXPECT_EQ(report.fields[i].label, expected_fields[i].label);
        EXPECT_EQ(report.fields[i].value, expected_fields[i].value);
        EXPECT_EQ(report.fields[i].is_code, expected_fields[i].is_code);
    }
}

static void write_named_report(StringView name, StringView text)
{
    auto directory = MUST(Core::Directory::create(test_directory(), Core::Directory::CreateDirectories::Yes));
    auto file = MUST(directory.open(name, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    MUST(file->write_until_depleted(text.bytes()));
}

TEST_CASE(the_crash_date_is_read_from_the_report_and_not_from_its_name)
{
    cleanup();
    ScopeGuard guard = cleanup;

    VERIFY(setenv("TZ", "XYZ-2", 1) == 0);
    tzset();

    // Reports saved before names carried the time of the crash have no time to take from their names.
    write_named_report("WebContent-legacy.txt"sv, report_text);
    WebView::CrashReportStore store { test_directory() };
    CrashReportReview review { store };
    auto report = MUST(review.open(ByteString { "WebContent-legacy.txt"sv }));

    EXPECT_EQ(report.fields[0].label, "Crash date"sv);
    EXPECT_EQ(report.fields[0].value, "2026-10-01 14:34:56"sv);
}

TEST_CASE(a_report_that_does_not_say_when_it_crashed_has_no_crash_date)
{
    cleanup();
    ScopeGuard guard = cleanup;

    write_named_report("WebContent-legacy.txt"sv, "Ladybird crash report, format 1\nProcess: WebContent\nCrashed at: yesterday\n"sv);
    WebView::CrashReportStore store { test_directory() };
    CrashReportReview review { store };
    auto report = MUST(review.open(ByteString { "WebContent-legacy.txt"sv }));

    for (auto const& field : report.fields)
        EXPECT(field.label != "Crash date"sv);
}

TEST_CASE(an_opened_report_is_no_longer_pending)
{
    cleanup();
    ScopeGuard guard = cleanup;

    auto older = write_report("2026-10-01T12-00-00Z"sv);
    auto newer = write_report("2026-10-01T13-00-00Z"sv);
    WebView::CrashReportStore store { test_directory() };
    CrashReportReview review { store };

    // Without a name, the newest report waiting for an answer is shown.
    EXPECT_EQ(MUST(review.open()).name, newer);
    EXPECT(!is_pending(newer));
    EXPECT(review.has_next_report());

    EXPECT_EQ(MUST(review.open()).name, older);
    EXPECT(!is_pending(older));
    EXPECT(!review.has_next_report());

    EXPECT(review.open().is_error());
}

TEST_CASE(a_report_can_be_reopened_by_name_once_seen)
{
    cleanup();
    ScopeGuard guard = cleanup;

    auto name = write_report("2026-10-01T12-00-00Z"sv);
    WebView::CrashReportStore store { test_directory() };
    CrashReportReview review { store };

    MUST(review.open(name));
    EXPECT_EQ(MUST(review.open(name)).name, name);
}

TEST_CASE(choices_that_cannot_be_sent_are_explained)
{
    EXPECT(!CrashReportReview::validate(""sv, {}).has_value());
    EXPECT(!CrashReportReview::validate("I clicked a link"sv, "https://example.com/"sv).has_value());
    EXPECT(!CrashReportReview::validate(""sv, "HTTP://example.com/"sv).has_value());

    EXPECT_EQ(CrashReportReview::validate(""sv, ""sv), "Enter a website URL or leave the option unchecked."sv);
    EXPECT_EQ(CrashReportReview::validate(""sv, "file:///etc/passwd"sv), "Enter an http or https URL."sv);
    EXPECT_EQ(CrashReportReview::validate(""sv, "example.com"sv), "Enter an http or https URL."sv);

    auto long_description = ByteString::repeated('a', CrashReportReview::maximum_description_bytes + 1);
    EXPECT_EQ(CrashReportReview::validate(long_description, {}), "This description is too long to include in a report."sv);
    EXPECT(!CrashReportReview::validate(long_description.substring_view(1), {}).has_value());

    auto long_url = ByteString::formatted("https://{}", ByteString::repeated('a', CrashReportReview::maximum_url_bytes));
    EXPECT_EQ(CrashReportReview::validate(""sv, long_url.view()), "This website URL is too long to include in a report."sv);
}

TEST_CASE(a_report_that_cannot_be_sent_is_not_sent)
{
    cleanup();
    ScopeGuard guard = cleanup;

    auto name = write_report("2026-10-01T12-00-00Z"sv);
    WebView::CrashReportStore store { test_directory() };
    CrashReportReview review { store };
    MUST(review.open(name));

    auto failed = false;
    review.on_failed = [&](auto, String const&) { failed = true; };

    EXPECT_EQ(review.send(""_string, ""_string), "Enter a website URL or leave the option unchecked."sv);
    EXPECT(!review.is_sending());
    EXPECT(!failed);
}

TEST_CASE(a_report_changed_after_it_was_reviewed_is_not_sent)
{
    cleanup();
    ScopeGuard guard = cleanup;

    auto name = write_report("2026-10-01T12-00-00Z"sv);
    WebView::CrashReportStore store { test_directory() };
    CrashReportReview review { store };
    MUST(review.open(name));

    auto seen_directory = LexicalPath::join(test_directory(), "Seen"sv).string();
    auto directory = MUST(Core::Directory::create(seen_directory, Core::Directory::CreateDirectories::No));
    auto file = MUST(directory.open(name, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    MUST(file->write_until_depleted("Ladybird crash report, format 1\nProcess: Browser\n"sv.bytes()));

    Optional<CrashReportSubmission::Failure> failure;
    String reason;
    review.on_failed = [&](auto kind, String message) {
        failure = kind;
        reason = move(message);
    };

    EXPECT(!review.send(""_string, {}).has_value());
    EXPECT(failure == CrashReportSubmission::Failure::Preparation);
    EXPECT_EQ(reason, "This report changed after it was reviewed, so it was not sent."sv);
    EXPECT(!review.is_sending());
}
