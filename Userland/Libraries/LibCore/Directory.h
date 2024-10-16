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
#include <AK/IterationDecision.h>
#include <AK/LexicalPath.h>
#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <LibCore/DirIterator.h>
#include <LibCore/DirectoryEntry.h>
#include <LibCore/File.h>
#include <sys/stat.h>
#if defined(AK_OS_WINDOWS)
#    include <windows.h>
#else
#    include <dirent.h>
#endif

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

    ErrorOr<NonnullOwnPtr<File>> open(StringView filename, File::OpenMode mode) const;
    ErrorOr<struct stat> stat(StringView filename, int flags) const;
    ErrorOr<struct stat> stat() const;

    LexicalPath const& path() const { return m_path; }

    using ForEachEntryCallback = Function<ErrorOr<IterationDecision>(DirectoryEntry const&, Directory const& parent)>;
    static ErrorOr<void> for_each_entry(StringView path, DirIterator::Flags, ForEachEntryCallback);
    ErrorOr<void> for_each_entry(DirIterator::Flags, ForEachEntryCallback);
#if !defined(AK_OS_WINDOWS)
    ErrorOr<void> chown(uid_t, gid_t);

    int fd() const { return m_directory_fd; }
    static ErrorOr<Directory> adopt_fd(int fd, LexicalPath);
    static ErrorOr<bool> is_valid_directory(int fd);
#else
    HANDLE handle() const
    {
        return m_directory_handle;
    }
    static ErrorOr<Directory> adopt_handle(HANDLE handle, LexicalPath);
    static ErrorOr<bool> is_valid_directory(HANDLE handle);
#endif

private:
#if !defined(AK_OS_WINDOWS)
    Directory(int directory_fd, LexicalPath path);
#else
    Directory(HANDLE directory_handle, LexicalPath path);
#endif
    static ErrorOr<void> ensure_directory(LexicalPath const& path, mode_t creation_mode = 0755);

    LexicalPath m_path;
#if !defined(AK_OS_WINDOWS)
    int m_directory_fd;
#else
    HANDLE m_directory_handle;
#endif
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
