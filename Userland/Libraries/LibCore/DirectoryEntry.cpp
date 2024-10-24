/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "DirectoryEntry.h"
#include <dirent.h>
#include <sys/stat.h>

namespace Core {

StringView DirectoryEntry::posix_name_from_directory_entry_type(Type type)
{
    switch (type) {
    case Type::BlockDevice:
        return "DT_BLK"sv;
    case Type::CharacterDevice:
        return "DT_CHR"sv;
    case Type::Directory:
        return "DT_DIR"sv;
    case Type::File:
        return "DT_REG"sv;
    case Type::NamedPipe:
        return "DT_FIFO"sv;
    case Type::Socket:
        return "DT_SOCK"sv;
    case Type::SymbolicLink:
        return "DT_LNK"sv;
    case Type::Unknown:
        return "DT_UNKNOWN"sv;
    case Type::Whiteout:
        return "DT_WHT"sv;
    }
    VERIFY_NOT_REACHED();
}

StringView DirectoryEntry::representative_name_from_directory_entry_type(Type type)
{
    switch (type) {
    case Type::BlockDevice:
        return "BlockDevice"sv;
    case Type::CharacterDevice:
        return "CharacterDevice"sv;
    case Type::Directory:
        return "Directory"sv;
    case Type::File:
        return "File"sv;
    case Type::NamedPipe:
        return "NamedPipe"sv;
    case Type::Socket:
        return "Socket"sv;
    case Type::SymbolicLink:
        return "SymbolicLink"sv;
    case Type::Unknown:
        return "Unknown"sv;
    case Type::Whiteout:
        return "Whiteout"sv;
    }
    VERIFY_NOT_REACHED();
}

DirectoryEntry::Type DirectoryEntry::directory_entry_type_from_stat(mode_t st_mode)
{
    switch (st_mode & S_IFMT) {
    case S_IFIFO:
        return DirectoryEntry::Type::NamedPipe;
    case S_IFCHR:
        return DirectoryEntry::Type::CharacterDevice;
    case S_IFDIR:
        return DirectoryEntry::Type::Directory;
    case S_IFREG:
        return DirectoryEntry::Type::File;
    case S_IFLNK:
        return DirectoryEntry::Type::SymbolicLink;
#ifndef AK_OS_WINDOWS
    case S_IFBLK:
        return DirectoryEntry::Type::BlockDevice;
    case S_IFSOCK:
        return DirectoryEntry::Type::Socket;
#endif
    default:
        return DirectoryEntry::Type::Unknown;
    }
    VERIFY_NOT_REACHED();
}

#if !defined(AK_OS_SOLARIS) && !defined(AK_OS_HAIKU)
static DirectoryEntry::Type directory_entry_type_from_posix(int dt_constant)
{
    switch (dt_constant) {
    case DT_UNKNOWN:
        return DirectoryEntry::Type::Unknown;
    case DT_FIFO:
        return DirectoryEntry::Type::NamedPipe;
    case DT_CHR:
        return DirectoryEntry::Type::CharacterDevice;
    case DT_DIR:
        return DirectoryEntry::Type::Directory;
    case DT_REG:
        return DirectoryEntry::Type::File;
    case DT_LNK:
        return DirectoryEntry::Type::SymbolicLink;
#    ifndef AK_OS_WINDOWS
    case DT_BLK:
        return DirectoryEntry::Type::BlockDevice;
    case DT_SOCK:
        return DirectoryEntry::Type::Socket;
#    endif
#    if !defined AK_OS_OPENBSD && !defined AK_OS_WINDOWS
    case DT_WHT:
        return DirectoryEntry::Type::Whiteout;
#    endif
    }
    VERIFY_NOT_REACHED();
}
#endif

#ifndef AK_OS_WINDOWS
DirectoryEntry DirectoryEntry::from_stat(DIR* d, dirent const& de)
{
    struct stat statbuf;
    fstat(dirfd(d), &statbuf);
    return DirectoryEntry {
        .type = directory_entry_type_from_stat(statbuf.st_mode),
        .name = de.d_name,
        .inode_number = de.d_ino,
    };
}
#endif

#if !defined(AK_OS_SOLARIS) && !defined(AK_OS_HAIKU)
DirectoryEntry DirectoryEntry::from_dirent(dirent const& de)
{
    return DirectoryEntry {
        .type = directory_entry_type_from_posix(de.d_type),
        .name = de.d_name,
        .inode_number = (ino_t)de.d_ino,
    };
}
#endif

}
