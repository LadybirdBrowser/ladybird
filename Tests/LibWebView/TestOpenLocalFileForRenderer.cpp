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
#include <LibTest/TemporaryDirectory.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Utilities.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

class TestDirectory {
public:
    TestDirectory()
        : m_root(MUST(FileSystem::real_path(m_directory.path().string())))
    {
    }

    ByteString const& root() const { return m_root; }
    ByteString join(StringView name) const { return LexicalPath::join(m_root, name).string(); }

    ByteString write_file(StringView name, StringView content) const
    {
        auto path = join(name);
        auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write));
        MUST(file->write_until_depleted(content));
        return path;
    }

    Vector<WebView::LocalFileRoot> roots() const { return { { m_root, WebView::LocalFileRoot::Scope::Directory } }; }

private:
    Test::TemporaryDirectory m_directory { "TestOpenLocalFileForRenderer"sv };
    ByteString m_root;
};

}

static void expect_refused(ByteString const& path, ReadonlySpan<WebView::LocalFileRoot> roots)
{
    if (!WebView::open_local_file_for_renderer(path, roots).is_error())
        FAIL(ByteString::formatted("{} was opened", path));
}

TEST_CASE(files_under_a_root_open)
{
    TestDirectory directory;
    auto file_path = directory.write_file("file.txt"sv, "content"sv);

    TRY_OR_FAIL(WebView::open_local_file_for_renderer(file_path, directory.roots()));
    TRY_OR_FAIL(WebView::open_local_file_for_renderer(directory.root(), directory.roots()));

    // A link inside the root to a file inside it reaches the file.
    auto link_path = directory.join("link.txt"sv);
    TRY_OR_FAIL(Core::System::symlink(file_path, link_path));
    auto linked = TRY_OR_FAIL(WebView::open_local_file_for_renderer(link_path, directory.roots()));
    EXPECT_EQ(linked.canonical_path, file_path);
    auto linked_file = TRY_OR_FAIL(Core::File::adopt_fd(linked.file.take_fd(), Core::File::OpenMode::Read));
    auto content = TRY_OR_FAIL(linked_file->read_until_eof());
    EXPECT_EQ(StringView { content }, "content"sv);
}

TEST_CASE(files_outside_the_roots_are_refused)
{
    TestDirectory directory;
    TestDirectory other_directory;
    auto other_file = other_directory.write_file("file.txt"sv, "content"sv);
    expect_refused(other_file, directory.roots());
    TRY_OR_FAIL(WebView::open_local_file_the_user_chose(other_file));

    // A link inside the root leads only to what's inside it too.
    auto link_path = directory.join("link"sv);
    TRY_OR_FAIL(Core::System::symlink(other_file, link_path));
    expect_refused(link_path, directory.roots());

    // As does a path that leaves the root and comes back elsewhere.
    expect_refused(ByteString::formatted("{}/../{}/file.txt", directory.root(), LexicalPath::basename(other_directory.root())), directory.roots());

    expect_refused("/etc/hosts"sv, directory.roots());
    expect_refused(directory.root(), {});
}

TEST_CASE(a_file_root_covers_only_its_file)
{
    TestDirectory directory;
    auto file_path = directory.write_file("file.txt"sv, "content"sv);
    auto sibling_path = directory.write_file("sibling.txt"sv, "content"sv);

    Vector<WebView::LocalFileRoot> roots { { file_path, WebView::LocalFileRoot::Scope::File } };
    TRY_OR_FAIL(WebView::open_local_file_for_renderer(file_path, roots));
    expect_refused(sibling_path, roots);
    expect_refused(directory.root(), roots);
}

TEST_CASE(other_kinds_of_file_are_refused)
{
    TestDirectory directory;

    auto fifo_path = directory.join("fifo"sv);
    VERIFY(mkfifo(fifo_path.characters(), 0600) == 0);
    expect_refused(fifo_path, directory.roots());

    auto link_path = directory.join("link"sv);
    TRY_OR_FAIL(Core::System::symlink(fifo_path, link_path));
    expect_refused(link_path, directory.roots());

    if (!WebView::open_local_file_the_user_chose("/dev/null"sv).is_error())
        FAIL("/dev/null was opened");
}

// A descriptor link reaches a file this process has open, wherever that file is.
TEST_CASE(descriptor_links_reach_only_files_under_the_roots)
{
    TestDirectory directory;
    TestDirectory other_directory;
    auto other_path = other_directory.write_file("file.txt"sv, "content"sv);
    auto other_file = TRY_OR_FAIL(Core::File::open(other_path, Core::File::OpenMode::Read));

    auto descriptor_path = ByteString::formatted("/dev/fd/{}", other_file->fd());
    expect_refused(descriptor_path, directory.roots());

    auto link_path = directory.join("link"sv);
    TRY_OR_FAIL(Core::System::symlink(descriptor_path, link_path));
    expect_refused(link_path, directory.roots());
}

#if defined(AK_OS_MACOS)
// A /dev/fd entry reads as the file behind it, so it's refused even under a root that covers it, and even when the user
// chose it. realpath() turns /dev/fd/N into /dev/fd/ and the file's name, so the file is named after its descriptor.
TEST_CASE(descriptor_entries_are_refused)
{
    static constexpr int descriptor = 900;
    // macOS starts a process with a limit of 256 descriptors.
    TRY_OR_FAIL(Core::System::set_resource_limits(RLIMIT_NOFILE, 65536));
    TestDirectory other_directory;
    auto other_file = TRY_OR_FAIL(Core::File::open(other_directory.write_file(ByteString::number(descriptor), "content"sv), Core::File::OpenMode::Read));
    VERIFY(dup2(other_file->fd(), descriptor) == descriptor);
    ScopeGuard close_descriptor = [] { close(descriptor); };
    auto descriptor_path = ByteString::formatted("/dev/fd/{}", descriptor);

    Vector<WebView::LocalFileRoot> roots { { "/dev/fd"sv, WebView::LocalFileRoot::Scope::Directory } };
    expect_refused(descriptor_path, roots);
    if (!WebView::open_local_file_the_user_chose(descriptor_path).is_error())
        FAIL(ByteString::formatted("{} was opened", descriptor_path));
}
#endif

#if defined(AK_OS_LINUX)
TEST_CASE(process_and_system_information_is_refused)
{
    for (auto path : { "/proc/self/maps"sv, "/proc/self/environ"sv, "/sys/kernel/mm/transparent_hugepage/enabled"sv }) {
        if (!WebView::open_local_file_the_user_chose(path).is_error())
            FAIL(ByteString::formatted("{} was opened", path));
    }
}
#endif
