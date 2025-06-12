/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <LibCore/DirIterator.h>
#include <LibFileSystem/FileSystem.h>

#if !defined(AK_OS_WINDOWS)
#    include <fcntl.h>
#    include <sys/stat.h>
#endif

namespace Test {

inline double get_time_in_ms()
{
    auto now = UnixDateTime::now();
    return static_cast<double>(now.milliseconds_since_epoch());
}

template<typename Callback>
inline void iterate_directory_recursively(ByteString const& directory_path, Callback callback)
{
    Core::DirIterator directory_iterator(directory_path, Core::DirIterator::Flags::SkipDots);

    while (directory_iterator.has_next()) {
        auto name = directory_iterator.next_path();
        auto full_path = LexicalPath::join(directory_path, name).string();
#if defined(AK_OS_WINDOWS)
        bool is_directory = FileSystem::is_directory(full_path);
#else
        struct stat st = {};
        if (fstatat(directory_iterator.fd(), name.characters(), &st, AT_SYMLINK_NOFOLLOW) < 0)
            continue;
        bool is_directory = S_ISDIR(st.st_mode);
#endif
        if (is_directory && name != "/Fixtures"sv) {
            iterate_directory_recursively(full_path, callback);
        } else if (!is_directory) {
            callback(full_path);
        }
    }
}

}
