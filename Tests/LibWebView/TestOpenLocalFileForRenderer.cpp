/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Utilities.h>
#include <stdlib.h>

// A renderer names the file it wants by path, and the UI process opens it as itself. So a path that reaches one of this
// process's own descriptors, or a file whose content depends on the process reading it, is refused, however it's named.

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        char path_template[] = "/tmp/TestOpenLocalFileForRenderer.XXXXXX";
        VERIFY(mkdtemp(path_template));
        m_path = path_template;
    }

    ~TemporaryDirectory()
    {
        (void)FileSystem::remove(m_path, FileSystem::RecursionMode::Allowed);
    }

    ByteString const& path() const { return m_path; }
    ByteString join(StringView name) const { return LexicalPath::join(m_path, name).string(); }

private:
    ByteString m_path;
};

}

static void expect_refused(ByteString const& path)
{
    if (!WebView::open_local_file_for_renderer(path).is_error())
        FAIL(ByteString::formatted("{} was opened", path));
}

TEST_CASE(regular_files_and_directories_open)
{
    TemporaryDirectory directory;
    auto file_path = directory.join("file.txt"sv);
    auto file = TRY_OR_FAIL(Core::File::open(file_path, Core::File::OpenMode::Write));
    TRY_OR_FAIL(file->write_until_depleted("content"sv));

    TRY_OR_FAIL(WebView::open_local_file_for_renderer(file_path));
    TRY_OR_FAIL(WebView::open_local_file_for_renderer(directory.path()));

    // A symbolic link to a regular file reaches the file.
    auto link_path = directory.join("link.txt"sv);
    TRY_OR_FAIL(Core::System::symlink(file_path, link_path));
    auto linked = TRY_OR_FAIL(WebView::open_local_file_for_renderer(link_path));
    auto linked_file = TRY_OR_FAIL(Core::File::adopt_fd(linked.take_fd(), Core::File::OpenMode::Read));
    auto content = TRY_OR_FAIL(linked_file->read_until_eof());
    EXPECT_EQ(StringView { content }, "content"sv);
}

TEST_CASE(descriptor_links_are_refused)
{
    TemporaryDirectory directory;
    auto file_path = directory.join("file.txt"sv);
    auto file = TRY_OR_FAIL(Core::File::open(file_path, Core::File::OpenMode::ReadWrite));
    TRY_OR_FAIL(file->write_until_depleted("content"sv));

    // A descriptor link opens this process's own descriptor, even one for a file that's no longer at any path.
    TRY_OR_FAIL(Core::System::unlink(file_path));
    auto descriptor_path = ByteString::formatted("/dev/fd/{}", file->fd());
    expect_refused(descriptor_path);

    auto link_path = directory.join("link"sv);
    TRY_OR_FAIL(Core::System::symlink(descriptor_path, link_path));
    expect_refused(link_path);

    expect_refused("/dev/null"sv);
}

#if defined(AK_OS_LINUX)
TEST_CASE(process_and_system_information_is_refused)
{
    expect_refused("/proc/self/maps"sv);
    expect_refused("/proc/self/environ"sv);
    expect_refused("/proc/cpuinfo"sv);
    expect_refused("/sys/kernel/mm/transparent_hugepage/enabled"sv);

    TemporaryDirectory directory;
    auto link_path = directory.join("link"sv);
    TRY_OR_FAIL(Core::System::symlink("/proc/self/maps"sv, link_path));
    expect_refused(link_path);
}
#endif
