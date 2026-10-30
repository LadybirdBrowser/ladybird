/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <stdlib.h>

static ByteString make_home_with_user_dirs_file(StringView contents)
{
    // mkdtemp() creates a private directory atomically, under a name nobody could have set up beforehand.
    auto template_path = LexicalPath::join(Core::StandardPaths::tempfile_directory(), "test-libcore-standard-paths-XXXXXX"sv).string();
    auto buffer = MUST(ByteBuffer::copy(template_path.bytes()));
    buffer.append('\0');
    auto* created = mkdtemp(reinterpret_cast<char*>(buffer.data()));
    VERIFY(created);
    ByteString home { created };

    auto config = MUST(Core::Directory::create(LexicalPath::join(home, ".config"sv), Core::Directory::CreateDirectories::Yes));
    auto file = MUST(config.open("user-dirs.dirs"sv, Core::File::OpenMode::Write));
    MUST(file->write_until_depleted(contents.bytes()));

    MUST(Core::Environment::set("HOME"sv, home, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::unset("XDG_CONFIG_HOME"sv));
    MUST(Core::Environment::unset("XDG_DESKTOP_DIR"sv));
    MUST(Core::Environment::unset("XDG_DOWNLOAD_DIR"sv));
    MUST(Core::Environment::unset("XDG_DOCUMENTS_DIR"sv));
    MUST(Core::Environment::unset("XDG_MUSIC_DIR"sv));
    MUST(Core::Environment::unset("XDG_PICTURES_DIR"sv));
    return home;
}

TEST_CASE(user_dirs_file_sets_user_directories)
{
    auto home = make_home_with_user_dirs_file(R"~~~(# This file is written by xdg-user-dirs-update
XDG_DESKTOP_DIR="$HOME/Bureau"
XDG_DOWNLOAD_DIR="$HOME/T\"el\\echargements"
XDG_DOCUMENTS_DIR="/srv/documents"
XDG_PICTURES_DIR="$HOME/a\bc"
XDG_MUSIC_DIR=relative/music
XDG_DOWNLOAD_DIRECTORY="$HOME/Wrong"
)~~~"sv);

    EXPECT_EQ(Core::StandardPaths::desktop_directory(), LexicalPath::join(home, "Bureau"sv).string());
    EXPECT_EQ(Core::StandardPaths::downloads_directory(), LexicalPath::join(home, "T\"el\\echargements"sv).string());
    EXPECT_EQ(Core::StandardPaths::documents_directory(), "/srv/documents"sv);
    EXPECT_EQ(Core::StandardPaths::pictures_directory(), LexicalPath::join(home, "a\\bc"sv).string());

    // An unquoted or relative path isn't a valid entry, so the default directory stands in.
    EXPECT_EQ(Core::StandardPaths::music_directory(), LexicalPath::join(home, "Music"sv).string());

    MUST(FileSystem::remove(home, FileSystem::RecursionMode::Allowed));
}

TEST_CASE(last_valid_assignment_in_user_dirs_file_wins)
{
    auto home = make_home_with_user_dirs_file(R"~~~(XDG_DOWNLOAD_DIR="$HOME/Old"
XDG_DOWNLOAD_DIR="$HOME/New"
XDG_DOWNLOAD_DIR=not-quoted
)~~~"sv);

    EXPECT_EQ(Core::StandardPaths::downloads_directory(), LexicalPath::join(home, "New"sv).string());

    MUST(FileSystem::remove(home, FileSystem::RecursionMode::Allowed));
}

TEST_CASE(user_dirs_file_entry_can_end_in_a_comment)
{
    auto home = make_home_with_user_dirs_file(R"~~~(XDG_DOWNLOAD_DIR="$HOME/Hamtningar" # preferred folder
XDG_MUSIC_DIR="$HOME/Musik" trailing-text
)~~~"sv);

    EXPECT_EQ(Core::StandardPaths::downloads_directory(), LexicalPath::join(home, "Hamtningar"sv).string());

    // Anything but a comment after the closing quote makes the entry invalid.
    EXPECT_EQ(Core::StandardPaths::music_directory(), LexicalPath::join(home, "Music"sv).string());

    MUST(FileSystem::remove(home, FileSystem::RecursionMode::Allowed));
}

TEST_CASE(relative_xdg_config_home_is_ignored)
{
    auto home = make_home_with_user_dirs_file("XDG_DOWNLOAD_DIR=\"$HOME/Hamtningar\"\n"sv);
    MUST(Core::Environment::set("XDG_CONFIG_HOME"sv, "relative/config"sv, Core::Environment::Overwrite::Yes));

    EXPECT_EQ(Core::StandardPaths::downloads_directory(), LexicalPath::join(home, "Hamtningar"sv).string());

    MUST(Core::Environment::unset("XDG_CONFIG_HOME"sv));
    MUST(FileSystem::remove(home, FileSystem::RecursionMode::Allowed));
}

TEST_CASE(environment_variable_takes_precedence_over_user_dirs_file)
{
    auto home = make_home_with_user_dirs_file("XDG_DOWNLOAD_DIR=\"$HOME/Hamtningar\"\n"sv);
    MUST(Core::Environment::set("XDG_DOWNLOAD_DIR"sv, "/srv/downloads"sv, Core::Environment::Overwrite::Yes));

    EXPECT_EQ(Core::StandardPaths::downloads_directory(), "/srv/downloads"sv);

    MUST(Core::Environment::unset("XDG_DOWNLOAD_DIR"sv));
    EXPECT_EQ(Core::StandardPaths::downloads_directory(), LexicalPath::join(home, "Hamtningar"sv).string());

    MUST(FileSystem::remove(home, FileSystem::RecursionMode::Allowed));
}

TEST_CASE(user_dirs_file_in_xdg_config_home)
{
    auto home = make_home_with_user_dirs_file(""sv);
    auto config_home = LexicalPath::join(home, "elsewhere"sv).string();
    auto config = MUST(Core::Directory::create(config_home, Core::Directory::CreateDirectories::Yes));
    auto file = MUST(config.open("user-dirs.dirs"sv, Core::File::OpenMode::Write));
    MUST(file->write_until_depleted("XDG_DOWNLOAD_DIR=\"$HOME/Scaricati\"\n"sv.bytes()));
    MUST(Core::Environment::set("XDG_CONFIG_HOME"sv, config_home, Core::Environment::Overwrite::Yes));

    EXPECT_EQ(Core::StandardPaths::downloads_directory(), LexicalPath::join(home, "Scaricati"sv).string());

    MUST(Core::Environment::unset("XDG_CONFIG_HOME"sv));
    MUST(FileSystem::remove(home, FileSystem::RecursionMode::Allowed));
}
