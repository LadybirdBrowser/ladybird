/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>

static ErrorOr<Core::Directory> create_temp_directory()
{
    auto path = LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("test-libcore-directory-{}", Core::System::getpid()));
    return Core::Directory::create(path, Core::Directory::CreateDirectories::Yes);
}

TEST_CASE(directory_open_and_stat)
{
    constexpr auto text = "Hello from Core::Directory!"sv;
    ByteString directory_path;

    {
        auto directory = TRY_OR_FAIL(create_temp_directory());
        directory_path = directory.path().string();

        {
            auto file = TRY_OR_FAIL(directory.open("test-file.txt"sv, Core::File::OpenMode::Write));
            TRY_OR_FAIL(file->write_until_depleted(text.bytes()));
        }

        {
            auto file = TRY_OR_FAIL(directory.open("test-file.txt"sv, Core::File::OpenMode::Read));
            auto contents = TRY_OR_FAIL(file->read_until_eof());
            EXPECT_EQ(StringView { contents.bytes() }, text);
        }

        auto file_stat = TRY_OR_FAIL(directory.stat("test-file.txt"sv));
        EXPECT(S_ISREG(file_stat.st_mode));
        EXPECT_EQ(static_cast<size_t>(file_stat.st_size), text.length());

        auto directory_stat = TRY_OR_FAIL(directory.stat());
        EXPECT(S_ISDIR(directory_stat.st_mode));

        EXPECT(directory.stat("nonexistent-file.txt"sv).is_error());
        EXPECT(directory.open("nonexistent-file.txt"sv, Core::File::OpenMode::Read).is_error());
    }

    TRY_OR_FAIL(FileSystem::remove(directory_path, FileSystem::RecursionMode::Allowed));
}
