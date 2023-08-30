/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/StringView.h>
#include <sys/types.h>

namespace Core {

struct DirectoryEntry {
    enum class Type {
        BlockDevice,
        CharacterDevice,
        Directory,
        File,
        NamedPipe,
        Socket,
        SymbolicLink,
        Unknown,
        Whiteout,
    };
    Type type;
    // FIXME: Once we have a special Path string class, use that.
    ByteString name;
#if !defined(AK_OS_WINDOWS)
    ino_t inode_number;
#endif

    static StringView posix_name_from_directory_entry_type(Type);
    static StringView representative_name_from_directory_entry_type(Type);
};

}
