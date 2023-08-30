/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "DirectoryEntry.h"

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

}
