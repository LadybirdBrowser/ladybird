/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/CrashReportStore.h>
#include <sys/stat.h>
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

static Vector<ByteString> directory_entries()
{
    Vector<ByteString> names;
    Core::DirIterator iterator(test_directory(), Core::DirIterator::SkipDots);
    while (iterator.has_next())
        names.append(iterator.next_path());
    return names;
}

TEST_CASE(a_stored_report_has_a_recognized_name_and_the_expected_contents)
{
    cleanup();
    ScopeGuard guard = cleanup;

    MUST(test_store().store_report(WebView::ProcessType::WebContent, "A report\n"sv));
    auto entries = directory_entries();
    EXPECT_EQ(entries.size(), 1u);
    auto const& name = entries[0];
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

TEST_CASE(retention_drops_the_oldest_reports)
{
    cleanup();
    ScopeGuard guard = cleanup;

    auto store = test_store();
    auto directory = MUST(Core::Directory::create(test_directory(), Core::Directory::CreateDirectories::Yes));
    Vector<ByteString> names;
    for (u32 i = 0; i < 20; ++i) {
        MUST(store.store_report(WebView::ProcessType::WebContent, "A report\n"sv));
        ByteString name;
        for (auto const& entry : directory_entries()) {
            if (!names.contains_slow(entry)) {
                name = entry;
                break;
            }
        }
        VERIFY(!name.is_empty());
        timespec times[2] { { 1000 + i, 0 }, { 1000 + i, 0 } };
        auto file = MUST(directory.open(name, Core::File::OpenMode::Read));
        VERIFY(futimens(file->fd(), times) == 0);
        names.append(move(name));
    }

    // The directory is now at the retention limit, so storing one more has to evict the oldest.
    MUST(store.store_report(WebView::ProcessType::WebContent, "One more\n"sv));

    auto entries = directory_entries();
    EXPECT_EQ(entries.size(), 20u);
    EXPECT(!entries.contains_slow(names[0]));
    EXPECT(entries.contains_slow(names[1]));
}
