/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <LibCore/CrashReportData.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/CrashReport.h>
#include <LibWebView/CrashReportStore.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static ByteString test_directory()
{
    auto name = ByteString::formatted("test-crash-report-store-{}", getpid());
    return LexicalPath::join(Core::StandardPaths::tempfile_directory(), name).string();
}

static WebView::CrashReportStore test_store()
{
    return WebView::CrashReportStore { test_directory() };
}

static void cleanup()
{
    if (FileSystem::exists(test_directory()))
        MUST(FileSystem::remove(test_directory(), FileSystem::RecursionMode::Allowed));
}

static void write_file(StringView name, StringView contents)
{
    auto directory = MUST(Core::Directory::create(test_directory(), Core::Directory::CreateDirectories::Yes));
    auto file = MUST(directory.open(name, Core::File::OpenMode::Write));
    MUST(file->write_until_depleted(contents.bytes()));
}

// A name the store accepts: a UTC timestamp, a known process, and a six character suffix.
static ByteString report_name(StringView timestamp, StringView process = "WebContent"sv)
{
    return ByteString::formatted("{}-{}-abc123.txt", timestamp, process);
}

// The files in the store's directory, or in one of its subdirectories.
static Vector<ByteString> directory_entries(StringView subdirectory = {})
{
    auto path = LexicalPath::join(test_directory(), subdirectory).string();
    Vector<ByteString> names;
    Core::DirIterator iterator(path, Core::DirIterator::SkipDots);
    while (iterator.has_next()) {
        auto name = iterator.next_path();
        if (!FileSystem::is_directory(LexicalPath::join(path, name).string()))
            names.append(move(name));
    }
    return names;
}

TEST_CASE(a_stored_report_is_named_after_the_time_of_the_crash)
{
    cleanup();
    ScopeGuard guard = cleanup;

    auto crashed_at = UnixDateTime::from_seconds_since_epoch(1772000767);
    auto name = MUST(test_store().store_report(WebView::ProcessType::WebContent, "A report\n"sv, crashed_at));
    EXPECT(name.starts_with("2026-02-25T06-26-07Z-WebContent-"sv));
    EXPECT(name.ends_with(".txt"sv));
    EXPECT(WebView::CrashReportStore::is_saved_report_name(name));

    auto directory = MUST(Core::Directory::create(test_directory(), Core::Directory::CreateDirectories::No));
    EXPECT_EQ(ByteString::copy(MUST(MUST(directory.open(name, Core::File::OpenMode::Read))->read_until_eof())),
        "A report\n"sv);
}

TEST_CASE(unrelated_files_are_not_reports)
{
    EXPECT(!WebView::CrashReportStore::is_saved_report_name("notes.txt"sv));
    EXPECT(!WebView::CrashReportStore::is_saved_report_name("2026-03-04T05-06-07Z-Firefox-abc123.txt"sv));
    EXPECT(!WebView::CrashReportStore::is_saved_report_name("2026-03-04T05-06-07Z-WebContent-abc123.log"sv));
    EXPECT(WebView::CrashReportStore::is_saved_report_name("2026-03-04T05-06-07Z-WebContent-abc123.txt"sv));
}

TEST_CASE(a_name_cannot_reach_outside_the_store)
{
    EXPECT(!WebView::CrashReportStore::is_saved_report_name("WebContent-x/../a.txt"sv));
    EXPECT(!WebView::CrashReportStore::is_saved_report_name("2026-03-04T05-06-07Z-WebContent-ab/c12.txt"sv));
    EXPECT(!WebView::CrashReportStore::is_saved_report_name("2026-03-04T05-06-07Z-WebContent-abc.12.txt"sv));
}

TEST_CASE(pending_reports_are_listed_newest_first)
{
    cleanup();
    ScopeGuard guard = cleanup;

    write_file(report_name("2026-01-01T00-00-00Z"sv), "Older report\n"sv);
    write_file(report_name("2026-03-04T05-06-07Z"sv), "Newer report\n"sv);

    auto names = MUST(test_store().pending_report_names());
    EXPECT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], report_name("2026-03-04T05-06-07Z"sv));
    EXPECT_EQ(names[1], report_name("2026-01-01T00-00-00Z"sv));

    auto report = MUST(test_store().saved_report(names[0]));
    EXPECT_EQ(report.text, "Newer report\n"sv);
}

TEST_CASE(a_name_the_store_rejects_is_never_read_or_marked)
{
    cleanup();
    ScopeGuard guard = cleanup;

    write_file("notes.txt"sv, "Not a report\n"sv);
    EXPECT(MUST(test_store().pending_report_names()).is_empty());
    EXPECT(test_store().saved_report("notes.txt"sv).is_error());
    EXPECT(test_store().mark_seen("../escape.txt"sv).is_error());
    EXPECT(test_store().remove_sent_report("notes.txt"sv).is_error());
}

TEST_CASE(a_seen_report_is_no_longer_pending)
{
    cleanup();
    ScopeGuard guard = cleanup;

    auto name = report_name("2026-03-04T05-06-07Z"sv);
    write_file(name, "A report\n"sv);
    auto store = test_store();

    EXPECT(store.has_pending_reports());
    MUST(store.mark_seen(name));
    MUST(store.mark_seen(name));
    EXPECT(!store.has_pending_reports());
    EXPECT(directory_entries().is_empty());
    EXPECT_EQ(directory_entries("Seen"sv), Vector<ByteString> { name });

    // A seen report is left on disk, so it can still be read and sent later.
    EXPECT_EQ(MUST(store.saved_report(name)).text, "A report\n"sv);

    write_file(report_name("2026-03-05T05-06-07Z"sv), "Another report\n"sv);
    EXPECT(store.has_pending_reports());
}

TEST_CASE(a_missing_report_cannot_be_moved)
{
    cleanup();
    ScopeGuard guard = cleanup;

    auto name = report_name("2026-03-04T05-06-07Z"sv);
    auto store = test_store();
    EXPECT(store.mark_seen(name).is_error());
    EXPECT(store.saved_report(name).is_error());
}

TEST_CASE(sending_a_report_removes_it_wherever_it_is)
{
    cleanup();
    ScopeGuard guard = cleanup;

    auto pending_name = report_name("2026-03-04T05-06-07Z"sv);
    auto seen_name = report_name("2026-03-05T05-06-07Z"sv);
    write_file(pending_name, "A report\n"sv);
    write_file(seen_name, "Another report\n"sv);
    auto store = test_store();
    MUST(store.mark_seen(seen_name));

    MUST(store.remove_sent_report(pending_name));
    MUST(store.remove_sent_report(seen_name));
    EXPECT(directory_entries().is_empty());
    EXPECT(directory_entries("Seen"sv).is_empty());
}

TEST_CASE(retention_only_drops_the_oldest_seen_reports)
{
    cleanup();
    ScopeGuard guard = cleanup;

    auto store = test_store();
    auto directory = MUST(Core::Directory::create(test_directory(), Core::Directory::CreateDirectories::Yes));
    auto store_report_modified_at = [&](u32 index, time_t modified) {
        auto crashed_at = UnixDateTime::from_seconds_since_epoch(1772000000 + index);
        auto name = MUST(store.store_report(WebView::ProcessType::WebContent, "A report\n"sv, crashed_at));
        timespec times[2] { { modified, 0 }, { modified, 0 } };
        auto file = MUST(directory.open(name, Core::File::OpenMode::Read));
        VERIFY(futimens(file->fd(), times) == 0);
        return name;
    };

    // Older than every seen report, but still awaiting review.
    Vector<ByteString> awaiting_review;
    for (u32 i = 0; i < 3; ++i)
        awaiting_review.append(store_report_modified_at(i, 500 + i));

    // Reports saved before names began with the crash time are seen reports like any other.
    auto legacy_name = "WebContent-legacy.txt"sv;
    write_file(legacy_name, "A legacy report\n"sv);
    {
        timespec times[2] { { 1, 0 }, { 1, 0 } };
        auto file = MUST(directory.open(legacy_name, Core::File::OpenMode::Read));
        VERIFY(futimens(file->fd(), times) == 0);
    }
    MUST(store.mark_seen(legacy_name));

    Vector<ByteString> seen;
    for (u32 i = 0; i < 21; ++i) {
        seen.append(store_report_modified_at(100 + i, 1000 + i));
        MUST(store.mark_seen(seen.last()));
    }

    auto entries = directory_entries();
    EXPECT_EQ(entries.size(), awaiting_review.size());
    for (auto const& name : awaiting_review)
        EXPECT(entries.contains_slow(name));

    auto seen_entries = directory_entries("Seen"sv);
    EXPECT_EQ(seen_entries.size(), 20u);
    EXPECT(!seen_entries.contains_slow(legacy_name));
    EXPECT(!seen_entries.contains_slow(seen[0]));
    for (auto const& name : seen.span().slice(1))
        EXPECT(seen_entries.contains_slow(name));
}

// A record as a browser leaves it when it crashes, with the description of its build that it recorded when it started,
// if any.
static ByteString write_pending_report(int signal, time_t modified, Optional<StringView> build_description = {})
{
    Core::CrashReportData::ReportHeader header {};
    header.magic = Core::CrashReportData::report_magic;
    header.signal = signal;

    auto directory = MUST(Core::Directory::create(test_directory(), Core::Directory::CreateDirectories::Yes));
    auto name = "Browser-abc123.pending"sv;
    auto file = MUST(directory.open(name, Core::File::OpenMode::ReadWrite));
    MUST(file->write_until_depleted({ &header, sizeof(header) }));

    WebView::CrashReport record { move(file), WebView::ProcessType::Browser };
    if (build_description.has_value())
        MUST(record.record_build_description(*build_description));

    timespec times[2] { { modified, 0 }, { modified, 0 } };
    VERIFY(futimens(record.fd(), times) == 0);
    return name;
}

static ByteString recovered_report_text()
{
    auto entries = directory_entries();
    VERIFY(entries.size() == 1);
    auto directory = MUST(Core::Directory::create(test_directory(), Core::Directory::CreateDirectories::No));
    return ByteString::copy(MUST(MUST(directory.open(entries[0], Core::File::OpenMode::Read))->read_until_eof()));
}

TEST_CASE(the_report_being_offered_survives_retention)
{
    cleanup();
    ScopeGuard guard = cleanup;

    auto store = test_store();
    auto directory = MUST(Core::Directory::create(test_directory(), Core::Directory::CreateDirectories::Yes));
    auto write_report_modified_at = [&](StringView timestamp, time_t modified) {
        auto name = report_name(timestamp);
        write_file(name, "A report\n"sv);
        timespec times[2] { { modified, 0 }, { modified, 0 } };
        auto file = MUST(directory.open(name, Core::File::OpenMode::Read));
        VERIFY(futimens(file->fd(), times) == 0);
        return name;
    };

    // An older report still awaits review while the seen reports are at their limit.
    auto offered = write_report_modified_at("2026-01-01T00-00-00Z"sv, 500);
    Vector<ByteString> seen;
    for (u32 i = 0; i < 20; ++i) {
        seen.append(write_report_modified_at(ByteString::formatted("2026-02-01T00-00-{:02}Z", i), 1000 + i));
        MUST(store.mark_seen(seen.last()));
    }

    // The oldest of the other seen reports goes instead.
    MUST(store.mark_seen(offered));
    auto seen_entries = directory_entries("Seen"sv);
    EXPECT_EQ(seen_entries.size(), 20u);
    EXPECT(seen_entries.contains_slow(offered));
    EXPECT(!seen_entries.contains_slow(seen[0]));
    EXPECT_EQ(MUST(store.saved_report(offered)).text, "A report\n"sv);
}

TEST_CASE(a_recovered_browser_crash_keeps_the_time_it_crashed)
{
    cleanup();
    ScopeGuard guard = cleanup;

    write_pending_report(SIGSEGV, 1772000767);
    EXPECT_EQ(MUST(test_store().recover_pending_reports()), 1u);

    auto entries = directory_entries();
    EXPECT_EQ(entries.size(), 1u);
    // Not the time of this launch, which is what a report named after "now" would record.
    EXPECT(entries[0].starts_with("2026-02-25T06-26-07Z-Browser-"sv));

    auto directory = MUST(Core::Directory::create(test_directory(), Core::Directory::CreateDirectories::No));
    auto text = ByteString::copy(MUST(MUST(directory.open(entries[0], Core::File::OpenMode::Read))->read_until_eof()));
    // The report says so itself, so reading it needs nothing from its name.
    EXPECT(text.contains("\nCrashed at: 2026-02-25T06:26:07Z\n"sv));
    EXPECT(text.contains("Termination signal: SIGSEGV\nTermination signal number: 11\n"sv));
}

TEST_CASE(a_recovered_browser_crash_describes_the_build_that_crashed)
{
    cleanup();
    ScopeGuard guard = cleanup;

    // The launch that recovers a crash can be another build, for instance after an update.
    write_pending_report(SIGSEGV, 1772000767,
        "Version: 0.1-before-the-update\nPlatform: OldOS\nArchitecture: x86_64\nBuild configuration: release\n"sv);
    EXPECT_EQ(MUST(test_store().recover_pending_reports()), 1u);

    auto text = recovered_report_text();
    EXPECT(text.contains("Version: 0.1-before-the-update\nPlatform: OldOS\n"sv));
    EXPECT(!text.contains(WebView::CrashReport::describe_current_build(WebView::ProcessType::Browser)));
    EXPECT(!text.contains("unavailable, the browser"sv));
}

TEST_CASE(a_recovered_browser_crash_without_a_recorded_build_does_not_describe_the_recovering_one)
{
    cleanup();
    ScopeGuard guard = cleanup;

    write_pending_report(SIGSEGV, 1772000767);
    EXPECT_EQ(MUST(test_store().recover_pending_reports()), 1u);

    auto text = recovered_report_text();
    EXPECT(text.contains("Build information: unavailable, the browser that crashed did not record it\n"sv));
    EXPECT(!text.contains("Version: "sv));
    EXPECT(!text.contains("Platform: "sv));
}

TEST_CASE(a_recorded_build_that_is_not_plain_text_is_not_trusted)
{
    cleanup();
    ScopeGuard guard = cleanup;

    write_pending_report(SIGSEGV, 1772000767, "Version: 1.0\n\x1b[31mPlatform: injected\n"sv);
    EXPECT_EQ(MUST(test_store().recover_pending_reports()), 1u);

    auto text = recovered_report_text();
    EXPECT(text.contains("Build information: unavailable"sv));
    EXPECT(!text.contains("injected"sv));
}

TEST_CASE(a_pending_report_held_by_a_running_browser_is_left_alone)
{
    cleanup();
    ScopeGuard guard = cleanup;

    auto name = write_pending_report(SIGSEGV, 1772000767);
    auto directory = MUST(Core::Directory::create(test_directory(), Core::Directory::CreateDirectories::No));
    auto held = MUST(directory.open(name, Core::File::OpenMode::Read));

    // A live browser holds this lock for its lifetime. Process IDs get recycled, so they could not
    // tell a running browser from an unrelated process that inherited its number.
    VERIFY(flock(held->fd(), LOCK_EX | LOCK_NB) == 0);
    EXPECT_EQ(MUST(test_store().recover_pending_reports()), 0u);

    held->close();
    EXPECT_EQ(MUST(test_store().recover_pending_reports()), 1u);
}

TEST_CASE(a_pending_report_without_a_captured_signal_is_discarded)
{
    cleanup();
    ScopeGuard guard = cleanup;

    // A clean exit leaves no signal behind, and must not be reported as a crash.
    write_pending_report(0, 1772000767);
    EXPECT_EQ(MUST(test_store().recover_pending_reports()), 0u);
    EXPECT(directory_entries().is_empty());
}

TEST_CASE(a_browser_that_crashes_is_recovered_by_the_next_launch)
{
    cleanup();
    ScopeGuard guard = cleanup;

    // The crash happens in a child, which stands in for the browser. Crashing the browser application itself would
    // have the operating system report it to the user.
    auto store = test_store();
    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        if (store.initialize_browser_crash_handler().is_error())
            _exit(1);
        raise(SIGSEGV);
        _exit(1);
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    EXPECT(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);

    EXPECT_EQ(MUST(store.recover_pending_reports()), 1u);
    auto entries = directory_entries();
    EXPECT_EQ(entries.size(), 1u);
    EXPECT(entries[0].contains("-Browser-"sv));

    auto directory = MUST(Core::Directory::create(test_directory(), Core::Directory::CreateDirectories::No));
    auto text = ByteString::copy(MUST(MUST(directory.open(entries[0], Core::File::OpenMode::Read))->read_until_eof()));
    EXPECT(text.contains("Process: Browser\n"sv));
    EXPECT(text.contains(WebView::CrashReport::describe_current_build(WebView::ProcessType::Browser)));
    EXPECT(text.contains("Termination signal: SIGSEGV\nTermination signal number: 11\n"sv));
    EXPECT(text.contains("Native stack"sv));
}
