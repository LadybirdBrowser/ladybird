/*
 * Copyright (c) 2022, kleines Filmröllchen <filmroellchen@serenityos.org>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/Function.h>
#include <AK/LexicalPath.h>
#include <AK/Noncopyable.h>
#include <LibCore/DirIterator.h>
#include <LibCore/DirectoryEntry.h>
#include <LibCore/File.h>
#include <sys/stat.h>

namespace Core {

// Deal with real system directories. Any Directory instance always refers to a valid existing directory.
class Directory {
    AK_MAKE_NONCOPYABLE(Directory);

public:
    Directory(Directory&&);
    ~Directory();

    // When this flag is set, both the directory attempted to instantiate as well as all of its parents are created with mode 0755 if necessary.
    enum class CreateDirectories : bool {
        No,
        Yes,
    };

    static ErrorOr<Directory> create(LexicalPath path, CreateDirectories, mode_t creation_mode = 0755);
    static ErrorOr<Directory> create(ByteString path, CreateDirectories, mode_t creation_mode = 0755);
    static ErrorOr<Directory> adopt_fd(int fd, LexicalPath path);

    ErrorOr<NonnullOwnPtr<File>> open(StringView filename, File::OpenMode mode) const;
    ErrorOr<struct stat> stat(StringView filename, int flags) const;
    ErrorOr<struct stat> stat() const;
    int fd() const { return m_directory_fd; }

    LexicalPath const& path() const { return m_path; }

    using ForEachEntryCallback = Function<ErrorOr<IterationDecision>(DirectoryEntry const&, Directory const& parent)>;
    static ErrorOr<void> for_each_entry(StringView path, DirIterator::Flags, ForEachEntryCallback);
    ErrorOr<void> for_each_entry(DirIterator::Flags, ForEachEntryCallback);

#ifndef AK_OS_WINDOWS
    ErrorOr<void> chown(uid_t, gid_t);
#endif

    static ErrorOr<bool> is_valid_directory(int fd);

private:
    Directory(int directory_fd, LexicalPath path);
    static ErrorOr<void> ensure_directory(LexicalPath const& path, mode_t creation_mode = 0755);

    LexicalPath m_path;
    int m_directory_fd;
};

}

namespace AK {

template<>
struct Formatter<Core::Directory> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Core::Directory const& directory)
    {
        TRY(builder.put_string(directory.path().string()));
        return {};
    }
};

}
