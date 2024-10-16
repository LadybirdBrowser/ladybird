/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/DirectoryEntry.h>
#include <sys/stat.h>

namespace Core {

static DirectoryEntry::Type directory_entry_type_from_win32(DWORD file_attributes)
{
    if (file_attributes & FILE_ATTRIBUTE_DIRECTORY)
        return DirectoryEntry::Type::Directory;
    if (file_attributes & FILE_ATTRIBUTE_DEVICE)
        return DirectoryEntry::Type::CharacterDevice;
    if (file_attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        return DirectoryEntry::Type::SymbolicLink;
    return DirectoryEntry::Type::File;
}

DirectoryEntry DirectoryEntry::from_find_data(WIN32_FIND_DATA const& de)
{
    return DirectoryEntry {
        .type = directory_entry_type_from_win32(de.dwFileAttributes),
        .name = de.cFileName,
    };
}

}
