/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TemporaryDirectory.h>

namespace Test {

static LexicalPath create_directory(StringView name)
{
    static int count = 0;
    auto path = LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("{}-{}-{}", name, Core::System::getpid(), count++));

    // A crashed run with the same process ID can leave it behind.
    (void)FileSystem::remove(path.string(), FileSystem::RecursionMode::Allowed);
    MUST(Core::Directory::create(path, Core::Directory::CreateDirectories::Yes, 0700));

    // File watchers report resolved paths, and the temp directory can be behind a symbolic link.
    return LexicalPath { MUST(FileSystem::real_path(path.string())) };
}

TemporaryDirectory::TemporaryDirectory(StringView name)
    : m_path(create_directory(name))
{
}

TemporaryDirectory::~TemporaryDirectory()
{
    (void)FileSystem::remove(m_path.string(), FileSystem::RecursionMode::Allowed);
}

}
