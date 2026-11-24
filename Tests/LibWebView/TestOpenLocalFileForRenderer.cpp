/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibTest/TemporaryDirectory.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Utilities.h>
#include <sys/stat.h>
#include <unistd.h>

static ByteString write_file(Test::TemporaryDirectory const& directory, StringView name, StringView content)
{
    auto path = directory.path().append(name).string();
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write));
    MUST(file->write_until_depleted(content));
    return path;
}

static void expect_refused(ByteString const& path)
{
    if (!WebView::open_local_file_for_renderer(path).is_error())
        FAIL(ByteString::formatted("{} was opened", path));
}

TEST_CASE(regular_files_and_directories_open)
{
    Test::TemporaryDirectory directory { "TestOpenLocalFileForRenderer"sv };
    auto file_path = write_file(directory, "file.txt"sv, "content"sv);

    TRY_OR_FAIL(WebView::open_local_file_for_renderer(file_path));
    TRY_OR_FAIL(WebView::open_local_file_for_renderer(directory.path().string()));

    auto link_path = directory.path().append("link.txt"sv).string();
    TRY_OR_FAIL(Core::System::symlink(file_path, link_path));
    auto linked = TRY_OR_FAIL(WebView::open_local_file_for_renderer(link_path));
    auto linked_file = TRY_OR_FAIL(Core::File::adopt_fd(linked.take_fd(), Core::File::OpenMode::Read));
    auto content = TRY_OR_FAIL(linked_file->read_until_eof());
    EXPECT_EQ(StringView { content }, "content"sv);
}

TEST_CASE(other_kinds_of_file_are_refused)
{
    Test::TemporaryDirectory directory { "TestOpenLocalFileForRenderer"sv };

    auto fifo_path = directory.path().append("fifo"sv).string();
    VERIFY(mkfifo(fifo_path.characters(), 0600) == 0);
    expect_refused(fifo_path);

    auto link_path = directory.path().append("link"sv).string();
    TRY_OR_FAIL(Core::System::symlink(fifo_path, link_path));
    expect_refused(link_path);

    expect_refused("/dev/null"sv);
}

#if defined(AK_OS_MACOS)
// A /dev/fd entry opens a descriptor of this process, which reads as the file behind it. realpath() turns /dev/fd/N into
// /dev/fd/ and the file's name, so the file is named after its descriptor.
TEST_CASE(descriptor_entries_are_refused)
{
    static constexpr int descriptor = 900;
    // macOS starts a process with a limit of 256 descriptors.
    TRY_OR_FAIL(Core::System::set_resource_limits(RLIMIT_NOFILE, 65536));
    Test::TemporaryDirectory directory { "TestOpenLocalFileForRenderer"sv };
    auto file = TRY_OR_FAIL(Core::File::open(write_file(directory, ByteString::number(descriptor), "content"sv), Core::File::OpenMode::Read));
    VERIFY(dup2(file->fd(), descriptor) == descriptor);
    ScopeGuard close_descriptor = [] { close(descriptor); };
    expect_refused(ByteString::formatted("/dev/fd/{}", descriptor));
}
#endif

#if defined(AK_OS_LINUX)
TEST_CASE(process_and_system_information_is_refused)
{
    expect_refused("/proc/self/maps"sv);
    expect_refused("/proc/self/environ"sv);
    expect_refused("/sys/kernel/mm/transparent_hugepage/enabled"sv);
}
#endif
