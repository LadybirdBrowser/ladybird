/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibTest/TemporaryDirectory.h>
#include <LibTest/TestCase.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>

namespace {

// The paths given to open_regular_file_or_directory() are canonical, so the temporary directory's links are resolved.
class CanonicalDirectory {
public:
    CanonicalDirectory()
    {
        char resolved[PATH_MAX];
        VERIFY(realpath(m_directory.path().string().characters(), resolved));
        m_path = resolved;
    }

    ByteString join(StringView name) const { return LexicalPath::join(m_path, name).string(); }
    ByteString const& path() const { return m_path; }

    ByteString write_file(StringView name, StringView content) const
    {
        auto path = join(name);
        auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write));
        MUST(file->write_until_depleted(content));
        return path;
    }

private:
    Test::TemporaryDirectory m_directory { "TestLibCoreFile"sv };
    ByteString m_path;
};

}

static void expect_refused(StringView path)
{
    if (!Core::File::open_regular_file_or_directory(path).is_error())
        FAIL(ByteString::formatted("{} was opened", path));
}

TEST_CASE(regular_files_and_directories_open)
{
    CanonicalDirectory directory;
    auto file_path = directory.write_file("file.txt"sv, "content"sv);

    auto file = TRY_OR_FAIL(Core::File::open_regular_file_or_directory(file_path));
    auto content = TRY_OR_FAIL(file->read_until_eof());
    EXPECT_EQ(StringView { content }, "content"sv);
    EXPECT_EQ(fcntl(file->fd(), F_GETFL) & O_NONBLOCK, 0);

    EXPECT(!Core::File::open_regular_file_or_directory(directory.path()).is_error());
    EXPECT(!Core::File::open_regular_file_or_directory("/"sv).is_error());
}

TEST_CASE(directories_on_the_way_need_only_be_searchable)
{
    CanonicalDirectory directory;
    TRY_OR_FAIL(Core::System::mkdir(directory.join("search-only"sv), 0700));
    auto file_path = directory.write_file("search-only/file.txt"sv, "content"sv);
    TRY_OR_FAIL(Core::System::chmod(directory.join("search-only"sv), 0100));

    auto file = Core::File::open_regular_file_or_directory(file_path);
    TRY_OR_FAIL(Core::System::chmod(directory.join("search-only"sv), 0700));
    EXPECT(!file.is_error());
}

TEST_CASE(symbolic_links_are_refused)
{
    CanonicalDirectory directory;
    auto file_path = directory.write_file("file.txt"sv, "content"sv);

    auto file_link = directory.join("file-link"sv);
    TRY_OR_FAIL(Core::System::symlink(file_path, file_link));
    expect_refused(file_link);

    auto directory_link = directory.join("directory-link"sv);
    TRY_OR_FAIL(Core::System::symlink(directory.path(), directory_link));
    expect_refused(LexicalPath::join(directory_link, "file.txt"sv).string());
}

TEST_CASE(other_kinds_of_file_are_refused)
{
    CanonicalDirectory directory;
    auto fifo_path = directory.join("fifo"sv);
    VERIFY(mkfifo(fifo_path.characters(), 0600) == 0);
    expect_refused(fifo_path);

    expect_refused("/dev/null"sv);
}

TEST_CASE(relative_paths_are_refused)
{
    expect_refused("file.txt"sv);
}
