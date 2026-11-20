/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/LocalFileRoots.h>
#include <LibWebView/Utilities.h>
#include <stdlib.h>

// A renderer names the file it wants by path, and the UI process opens it as itself. So it opens only a file under the
// local file roots, and never one that reaches one of this process's own descriptors, or whose content depends on the
// process reading it, however the path names it.

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

static void expect_refused(ByteString const& path, WebView::LocalFileRootsOnly local_file_roots_only = WebView::LocalFileRootsOnly::Yes)
{
    if (!WebView::open_local_file_for_renderer(path, local_file_roots_only).is_error())
        FAIL(ByteString::formatted("{} was opened", path));
}

// Kernel interface files are refused even for a caller that may open files outside the roots, such as WebDriver.
static void expect_refused_for_any_caller(ByteString const& path)
{
    expect_refused(path, WebView::LocalFileRootsOnly::No);
    expect_refused(path, WebView::LocalFileRootsOnly::Yes);
}

// The temporary directory, where these tests make their files, is a local file root.
TEST_CASE(regular_files_and_directories_open)
{
    TemporaryDirectory directory;
    auto file_path = directory.join("file.txt"sv);
    auto file = TRY_OR_FAIL(Core::File::open(file_path, Core::File::OpenMode::Write));
    TRY_OR_FAIL(file->write_until_depleted("content"sv));

    TRY_OR_FAIL(WebView::open_local_file_for_renderer(file_path, WebView::LocalFileRootsOnly::Yes));
    TRY_OR_FAIL(WebView::open_local_file_for_renderer(directory.path(), WebView::LocalFileRootsOnly::Yes));

    // A symbolic link to a regular file reaches the file.
    auto link_path = directory.join("link.txt"sv);
    TRY_OR_FAIL(Core::System::symlink(file_path, link_path));
    auto linked = TRY_OR_FAIL(WebView::open_local_file_for_renderer(link_path, WebView::LocalFileRootsOnly::Yes));
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
    expect_refused_for_any_caller(descriptor_path);

    auto link_path = directory.join("link"sv);
    TRY_OR_FAIL(Core::System::symlink(descriptor_path, link_path));
    expect_refused_for_any_caller(link_path);

    expect_refused_for_any_caller("/dev/null"sv);
}

#if defined(AK_OS_LINUX)
TEST_CASE(process_and_system_information_is_refused)
{
    expect_refused_for_any_caller("/proc/self/maps"sv);
    expect_refused_for_any_caller("/proc/self/environ"sv);
    expect_refused_for_any_caller("/proc/cpuinfo"sv);
    expect_refused_for_any_caller("/sys/kernel/mm/transparent_hugepage/enabled"sv);

    TemporaryDirectory directory;
    auto link_path = directory.join("link"sv);
    TRY_OR_FAIL(Core::System::symlink("/proc/self/maps"sv, link_path));
    expect_refused_for_any_caller(link_path);
}
#endif

TEST_CASE(files_outside_the_roots_are_refused)
{
    // A system file, readable by anyone, that's under no local file root.
    auto system_file = TRY_OR_FAIL(FileSystem::real_path("/etc/hosts"sv));
    TRY_OR_FAIL(WebView::open_local_file_for_renderer(system_file, WebView::LocalFileRootsOnly::No));
    expect_refused(system_file);

    // A link under a root leads only to what's under a root too.
    TemporaryDirectory directory;
    auto link_path = directory.join("link"sv);
    TRY_OR_FAIL(Core::System::symlink(system_file, link_path));
    expect_refused(link_path);
}

TEST_CASE(roots_are_whole_directories)
{
    TemporaryDirectory directory;
    auto root = TRY_OR_FAIL(FileSystem::real_path(directory.path()));
    auto usual_roots = WebView::LocalFileRoots::replace_for_testing({ root });
    ScopeGuard restore_usual_roots = [&] { WebView::LocalFileRoots::replace_for_testing(move(usual_roots)); };

    EXPECT(WebView::LocalFileRoots::contains(root));
    EXPECT(WebView::LocalFileRoots::contains(LexicalPath::join(root, "file.txt"sv).string()));
    EXPECT(!WebView::LocalFileRoots::contains(ByteString::formatted("{}x/file.txt", root)));
    EXPECT(!WebView::LocalFileRoots::contains(LexicalPath::dirname(root)));

    // The file system's root directory is never a root, nor is anything in a kernel interface tree.
    WebView::LocalFileRoots::add("/"sv);
    EXPECT(!WebView::LocalFileRoots::contains("/usr"sv));
    WebView::LocalFileRoots::add("/dev"sv);
    EXPECT(!WebView::LocalFileRoots::contains("/dev/null"sv));
}
